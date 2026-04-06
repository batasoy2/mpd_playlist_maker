#include <iostream>
#include <filesystem>
#include <vector>
#include <string>
#include <algorithm>
#include <fstream>
#include <pwd.h>
#include <unistd.h>
#include <cstdlib>
#include <regex>

namespace fs = std::filesystem;

// Global silent flag
bool silent_mode = false;

// Silent cout wrapper
void silent_cout(const std::string& msg) {
    if (!silent_mode) {
        std::cout << msg << std::endl;
    }
}

// Silent cerr wrapper (always show errors, even in silent mode)
void silent_cerr(const std::string& msg) {
    std::cerr << msg << std::endl;
}

// Supported audio file extensions
const std::vector<std::string> AUDIO_EXTENSIONS = {
    ".mp3", ".flac", ".ogg", ".m4a", ".wav", ".opus", ".aac"
};

// Get current user's home directory
std::string get_user_home() {
    const char* home_dir = getenv("HOME");
    if (home_dir != nullptr) {
        return std::string(home_dir);
    }
    
    struct passwd* pw = getpwuid(getuid());
    if (pw != nullptr) {
        return std::string(pw->pw_dir);
    }
    
    return "";
}

// Read openpod.config and extract mpd_playlist_folder
std::string get_mpd_playlist_folder() {
    std::string home = get_user_home();
    if (home.empty()) {
        return "";
    }
    
    std::string config_path = home + "/openpod.config";
    std::ifstream config_file(config_path);
    
    if (!config_file.is_open()) {
        if (!silent_mode) {
            std::cerr << "Warning: Could not open " << config_path << std::endl;
        }
        return "";
    }
    
    std::string line;
    std::regex playlist_regex("mpd_playlist_folder\\s*=\\s*(.+)");
    std::smatch match;
    
    while (std::getline(config_file, line)) {
        if (std::regex_search(line, match, playlist_regex)) {
            std::string folder = match[1].str();
            
            // Remove quotes if present
            if (!folder.empty() && folder.front() == '"' && folder.back() == '"') {
                folder = folder.substr(1, folder.length() - 2);
            }
            
            // Remove leading/trailing whitespace
            folder = std::regex_replace(folder, std::regex("^\\s+|\\s+$"), "");
            
            // Expand ~ to home directory
            if (!folder.empty() && folder[0] == '~') {
                folder = get_user_home() + folder.substr(1);
            }
            
            config_file.close();
            return folder;
        }
    }
    
    config_file.close();
    return "";
}

// Find MPD's music directory from config (fallback if openpod.config doesn't specify)
std::string get_mpd_music_dir() {
    // First, try to get from openpod.config
    std::string playlist_folder = get_mpd_playlist_folder();
    if (!playlist_folder.empty()) {
        // MPD music directory is typically where playlists point to
        // We'll use the parent of playlist folder or a common default
        fs::path playlist_path(playlist_folder);
        fs::path music_path = playlist_path.parent_path();
        
        // Check if there's a common music directory
        std::vector<std::string> possible_music_dirs = {
            get_user_home() + "/Music",
            get_user_home() + "/music",
            "/var/lib/mpd/music",
            "/srv/mpd/music"
        };
        
        for (const auto& dir : possible_music_dirs) {
            if (fs::exists(dir)) {
                return dir;
            }
        }
    }
    
    // Fallback: try to read MPD config directly
    std::vector<std::string> config_paths = {
        get_user_home() + "/.config/mpd/mpd.conf",
        get_user_home() + "/.mpdconf",
        "/etc/mpd.conf"
    };
    
    for (const auto& config_path : config_paths) {
        std::ifstream config_file(config_path);
        if (config_file.is_open()) {
            std::string line;
            while (std::getline(config_file, line)) {
                if (line.find("music_directory") != std::string::npos) {
                    size_t start = line.find('"');
                    size_t end = line.rfind('"');
                    if (start != std::string::npos && end != std::string::npos && start < end) {
                        std::string music_dir = line.substr(start + 1, end - start - 1);
                        if (music_dir[0] == '~') {
                            music_dir = get_user_home() + music_dir.substr(1);
                        }
                        config_file.close();
                        return music_dir;
                    }
                }
            }
            config_file.close();
        }
    }
    
    // Default fallback: assume music is in ~/Music
    return get_user_home() + "/Music";
}

// Get folder name from path (handles trailing slashes and root folders)
std::string get_folder_name(const fs::path& folder_path) {
    // Get the filename component
    std::string folder_name = folder_path.filename().string();
    
    // If filename is "." or empty, use the parent folder name
    if (folder_name.empty() || folder_name == ".") {
        folder_name = folder_path.parent_path().filename().string();
    }
    
    // If still empty (e.g., root directory), use a default name
    if (folder_name.empty()) {
        folder_name = "Music";
    }
    
    return folder_name;
}

bool is_audio_file(const fs::path& file_path) {
    std::string extension = file_path.extension().string();
    std::transform(extension.begin(), extension.end(), extension.begin(), ::tolower);
    
    return std::find(AUDIO_EXTENSIONS.begin(), AUDIO_EXTENSIONS.end(), extension) 
           != AUDIO_EXTENSIONS.end();
}

// Convert absolute path to path relative to MPD's music directory
std::string get_relative_path_for_mpd(const fs::path& absolute_path, const fs::path& mpd_music_dir) {
    try {
        fs::path relative = fs::relative(absolute_path, mpd_music_dir);
        return relative.string();
    } catch (const std::exception& e) {
        if (!silent_mode) {
            std::cerr << "Warning: Could not make relative path for: " << absolute_path << std::endl;
        }
        return absolute_path.string();
    }
}

void scan_directory(const fs::path& directory, std::vector<fs::path>& audio_files, bool recursive) {
    try {
        if (recursive) {
            for (const auto& entry : fs::recursive_directory_iterator(directory)) {
                if (fs::is_regular_file(entry.path()) && is_audio_file(entry.path())) {
                    audio_files.push_back(entry.path());
                }
            }
        } else {
            for (const auto& entry : fs::directory_iterator(directory)) {
                if (fs::is_regular_file(entry.path()) && is_audio_file(entry.path())) {
                    audio_files.push_back(entry.path());
                }
            }
        }
    } catch (const fs::filesystem_error& e) {
        if (!silent_mode) {
            std::cerr << "Error reading directory: " << e.what() << std::endl;
        }
    }
}

void create_playlist(const fs::path& playlist_path, const std::vector<fs::path>& audio_files, const fs::path& mpd_music_dir) {
    if (audio_files.empty()) {
        if (!silent_mode) {
            std::cout << "No audio files found for playlist: " << playlist_path.stem().string() << std::endl;
        }
        return;
    }
    
    std::ofstream playlist_file(playlist_path);
    if (!playlist_file.is_open()) {
        if (!silent_mode) {
            std::cerr << "Error: Could not create playlist file: " << playlist_path << std::endl;
        }
        return;
    }
    
    for (const auto& file_path : audio_files) {
        playlist_file << get_relative_path_for_mpd(file_path, mpd_music_dir) << std::endl;
    }
    
    playlist_file.close();
    
    if (!silent_mode) {
        std::cout << "Created playlist: " << playlist_path.filename().string() << " (" << audio_files.size() << " tracks)" << std::endl;
    }
}

void process_subfolder(const fs::path& subfolder_path, const fs::path& mpd_music_dir, const fs::path& mpd_playlist_folder) {
    std::vector<fs::path> audio_files;
    
    // Scan subfolder recursively for all audio files
    scan_directory(subfolder_path, audio_files, true);
    
    if (!audio_files.empty()) {
        // Create playlist in MPD's playlist folder with subfolder's name
        std::string folder_name = get_folder_name(subfolder_path);
        fs::path playlist_filename = folder_name + ".m3u";
        fs::path playlist_path = mpd_playlist_folder / playlist_filename;
        create_playlist(playlist_path, audio_files, mpd_music_dir);
    }
}

void process_folder(const fs::path& folder_path, bool recursive, bool create_sub_playlists, 
                    const fs::path& mpd_music_dir, const fs::path& mpd_playlist_folder) {
    if (!fs::exists(folder_path) || !fs::is_directory(folder_path)) {
        if (!silent_mode) {
            std::cerr << "Error: '" << folder_path << "' is not a valid directory." << std::endl;
        }
        return;
    }
    
    // Ensure MPD playlist folder exists
    if (!fs::exists(mpd_playlist_folder)) {
        if (!silent_mode) {
            std::cout << "Creating MPD playlist folder: " << mpd_playlist_folder << std::endl;
        }
        try {
            fs::create_directories(mpd_playlist_folder);
        } catch (const fs::filesystem_error& e) {
            if (!silent_mode) {
                std::cerr << "Error creating playlist folder: " << e.what() << std::endl;
            }
            return;
        }
    }
    
    if (!silent_mode) {
        std::cout << "\nMPD Music Directory: " << mpd_music_dir << std::endl;
        std::cout << "MPD Playlist Folder: " << mpd_playlist_folder << std::endl;
        std::cout << "Processing folder: " << folder_path << "\n" << std::endl;
    }
    
    // Create playlist for the main folder (only top-level files, not subfolders)
    std::vector<fs::path> main_audio_files;
    scan_directory(folder_path, main_audio_files, false);
    
    if (!main_audio_files.empty()) {
        std::string folder_name = get_folder_name(folder_path);
        fs::path playlist_filename = folder_name + ".m3u";
        fs::path playlist_path = mpd_playlist_folder / playlist_filename;
        create_playlist(playlist_path, main_audio_files, mpd_music_dir);
    } else {
        if (!silent_mode) {
            std::cout << "No top-level audio files in: " << get_folder_name(folder_path) << std::endl;
        }
    }
    
    // If recursive flag is set, process each subfolder
    if (recursive && create_sub_playlists) {
        if (!silent_mode) {
            std::cout << "\nProcessing subfolders recursively..." << std::endl;
        }
        try {
            for (const auto& entry : fs::directory_iterator(folder_path)) {
                if (fs::is_directory(entry.path())) {
                    process_subfolder(entry.path(), mpd_music_dir, mpd_playlist_folder);
                }
            }
        } catch (const fs::filesystem_error& e) {
            if (!silent_mode) {
                std::cerr << "Error processing subfolders: " << e.what() << std::endl;
            }
        }
    }
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS] <folder_path>\n\n"
              << "Options:\n"
              << "  --recursive    Create separate playlists for each subfolder\n"
              << "  --silent       Suppress all output (errors still shown)\n"
              << "  --help         Display this help message\n\n"
              << "Description:\n"
              << "  Creates MPD-compatible .m3u playlist files with paths relative to MPD's music directory.\n"
              << "  Reads openpod.config for mpd_playlist_folder setting.\n"
              << "  Playlists are saved to the folder specified in openpod.config.\n"
              << "  Supported formats: mp3, flac, ogg, m4a, wav, opus, aac\n\n"
              << "Examples:\n"
              << "  " << program_name << " /home/batasoy/Music\n"
              << "  " << program_name << " --recursive /home/batasoy/Music\n"
              << "  " << program_name << " --silent --recursive ~/Music\n"
              << "  " << program_name << " ~/Music\n";
}

int main(int argc, char* argv[]) {
    bool recursive = false;
    std::string folder_path;
    
    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];
        
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--recursive") {
            recursive = true;
        } else if (arg == "--silent") {
            silent_mode = true;
        } else if (arg[0] != '-') {
            folder_path = arg;
        } else {
            std::cerr << "Unknown option: " << arg << "\n\n";
            print_usage(argv[0]);
            return 1;
        }
    }
    
    if (folder_path.empty()) {
        std::cerr << "Error: No folder path provided.\n\n";
        print_usage(argv[0]);
        return 1;
    }
    
    // Expand ~ to user's home directory if present
    if (folder_path[0] == '~') {
        std::string home = get_user_home();
        if (!home.empty()) {
            folder_path = home + folder_path.substr(1);
        }
    }
    
    fs::path path(folder_path);
    
    // Get MPD playlist folder from openpod.config
    std::string playlist_folder_str = get_mpd_playlist_folder();
    if (playlist_folder_str.empty()) {
        std::cerr << "Error: Could not find mpd_playlist_folder in openpod.config" << std::endl;
        std::cerr << "Please ensure " << get_user_home() << "/openpod.config contains:" << std::endl;
        std::cerr << "  mpd_playlist_folder=/path/to/mpd/playlists/" << std::endl;
        return 1;
    }
    
    fs::path mpd_playlist_folder(playlist_folder_str);
    
    // Get MPD's music directory for relative path calculation
    fs::path mpd_music_dir = get_mpd_music_dir();
    
    // Validate the folder exists
    if (!fs::exists(path)) {
        std::cerr << "Error: Folder does not exist: " << path << std::endl;
        return 1;
    }
    
    // Process the folder and create playlists
    process_folder(path, recursive, recursive, mpd_music_dir, mpd_playlist_folder);
    
    if (!silent_mode) {
        std::cout << "\n✓ Playlist creation complete!" << std::endl;
        std::cout << "Playlists saved to: " << mpd_playlist_folder << std::endl;
        std::cout << "\nTo use with MPD, run:\n" << std::endl;
        std::cout << "  mpc update" << std::endl;
        std::cout << "  mpc lsplaylists" << std::endl;
        std::cout << "  mpc load \"<playlist_name>\"" << std::endl;
        std::cout << "  mpc play" << std::endl;
    }
    
    return 0;
}
