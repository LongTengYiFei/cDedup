#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <random>
#include <filesystem>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <iomanip>
#include <algorithm>

namespace fs = std::filesystem;
using namespace std::chrono;

// 确保目录存在
void ensure_directory(const std::string& dir_path) {
    if (!fs::exists(dir_path)) {
        if (!fs::create_directory(dir_path)) {
            std::cerr << "Failed to create directory: " << dir_path << std::endl;
            exit(EXIT_FAILURE);
        }
        std::cout << "Created directory: " << dir_path << std::endl;
    }
}

// 生成随机文件内容（高效方式）
void create_random_file(const std::string& file_path, size_t size_mb) {
    const size_t size_bytes = size_mb * 1024 * 1024;
    std::vector<char> buffer(4096); // 4KB buffer

    // 使用/dev/urandom获取高质量随机数据
    int urandom_fd = open("/dev/urandom", O_RDONLY);
    if (urandom_fd < 0) {
        // 回退到伪随机数生成器
        std::random_device rd;
        std::mt19937 gen(rd());
        std::uniform_int_distribution<> dis(0, 255);
        
        std::ofstream file(file_path, std::ios::binary | std::ios::out);
        if (!file) {
            std::cerr << "Failed to open file: " << file_path << std::endl;
            exit(EXIT_FAILURE);
        }
        
        size_t written = 0;
        while (written < size_bytes) {
            const size_t chunk_size = std::min(size_t(4096), size_bytes - written);
            for (size_t i = 0; i < chunk_size; ++i) {
                buffer[i] = static_cast<char>(dis(gen));
            }
            file.write(buffer.data(), chunk_size);
            written += chunk_size;
        }
        return;
    }

    // 高效写入：使用4KB块
    std::ofstream file(file_path, std::ios::binary | std::ios::out);
    if (!file) {
        close(urandom_fd);
        std::cerr << "Failed to open file: " << file_path << std::endl;
        exit(EXIT_FAILURE);
    }

    size_t written = 0;
    while (written < size_bytes) {
        const size_t chunk_size = std::min(size_t(4096), size_bytes - written);
        ssize_t bytes_read = read(urandom_fd, buffer.data(), chunk_size);
        if (bytes_read <= 0) {
            close(urandom_fd);
            std::cerr << "Failed to read from /dev/urandom" << std::endl;
            exit(EXIT_FAILURE);
        }
        file.write(buffer.data(), bytes_read);
        written += bytes_read;
    }
    close(urandom_fd);
}

int main() {
    const int file_num = 128;
    const int file_size_mb = 4; // 4MB per file
    const std::string dir = "/home/cyf/ssd0/gc_overhead";
    const std::string file_prefix = "container_";

    // 1. 确保目录存在
    ensure_directory(dir);

    // 2. 创建随机文件
    std::cout << "Creating " << file_num << " random files (" << file_size_mb << "MB each)..." << std::endl;
    auto start_create = high_resolution_clock::now();
    
    std::vector<std::string> file_paths;
    for (int i = 0; i < file_num; ++i) {
        std::string file_path = dir + "/" + file_prefix + std::to_string(i);
        file_paths.push_back(file_path);
        create_random_file(file_path, file_size_mb);
    }
    
    auto end_create = high_resolution_clock::now();
    auto create_duration = duration_cast<microseconds>(end_create - start_create).count();
    std::cout << "File creation completed in " << create_duration/1000.0 << " ms" << std::endl;
    std::cout << "Average creation time per file: " << create_duration/(file_num*1000.0) << " ms" << std::endl;

    // 3. 同步文件系统以确保所有数据写入磁盘
    std::cout << "Syncing filesystem..." << std::endl;
    sync();

    // 4. 测量删除时间（精确到微秒）
    std::cout << "\nMeasuring deletion times (microseconds per file)..." << std::endl;
    std::vector<long> delete_times_us;
    delete_times_us.reserve(file_num);

    // 先清理可能存在的旧文件
    for (const auto& path : file_paths) {
        if (fs::exists(path)) {
            fs::remove(path);
        }
    }
    sync();

    // 重新创建文件用于精确测量
    for (int i = 0; i < file_num; ++i) {
        create_random_file(file_paths[i], file_size_mb);
    }
    sync();

    // 关键：禁用磁盘缓存以获得真实I/O时间
    std::cout << "Flushing disk caches..." << std::endl;
    system("echo 3 > /proc/sys/vm/drop_caches 2>/dev/null");

    // 精确测量每个文件的删除时间
    for (int i = 0; i < file_num; ++i) {
        const auto& file_path = file_paths[i];
        
        // 确保文件存在
        if (!fs::exists(file_path)) {
            std::cerr << "File not found: " << file_path << std::endl;
            continue;
        }

        // 高精度计时
        auto start = high_resolution_clock::now();
        bool success = fs::remove(file_path);
        auto end = high_resolution_clock::now();
        
        if (!success) {
            std::cerr << "Failed to delete file: " << file_path << std::endl;
            continue;
        }

        auto duration_us = duration_cast<microseconds>(end - start).count();
        delete_times_us.push_back(duration_us);
        
        // 每10个文件输出一次进度
        if ((i + 1) % 10 == 0 || i == file_num - 1) {
            std::cout << "  Deleted " << (i + 1) << "/" << file_num 
                      << " files. Last deletion: " << duration_us << " us" << std::endl;
        }
    }

    // 5. 计算统计结果
    if (delete_times_us.empty()) {
        std::cerr << "No files were successfully deleted!" << std::endl;
        return EXIT_FAILURE;
    }

    long min_time = *min_element(delete_times_us.begin(), delete_times_us.end());
    long max_time = *max_element(delete_times_us.begin(), delete_times_us.end());
    double avg_time = std::accumulate(delete_times_us.begin(), delete_times_us.end(), 0.0) / delete_times_us.size();
    
    // 计算中位数
    std::vector<long> sorted_times = delete_times_us;
    std::sort(sorted_times.begin(), sorted_times.end());
    double median_time;
    if (sorted_times.size() % 2 == 0) {
        median_time = (sorted_times[sorted_times.size()/2 - 1] + sorted_times[sorted_times.size()/2]) / 2.0;
    } else {
        median_time = sorted_times[sorted_times.size()/2];
    }
    
    // 计算99%分位数
    size_t p99_index = static_cast<size_t>(sorted_times.size() * 0.99);
    if (p99_index >= sorted_times.size()) p99_index = sorted_times.size() - 1;
    long p99_time = sorted_times[p99_index];

    // 6. 输出详细结果
    std::cout << "\n===== Deletion Time Statistics =====" << std::endl;
    std::cout << "Total files deleted: " << delete_times_us.size() << std::endl;
    std::cout << "File size: " << file_size_mb << " MB each" << std::endl;
    std::cout << "Directory: " << dir << std::endl;
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Min time: " << min_time << " us" << std::endl;
    std::cout << "Max time: " << max_time << " us" << std::endl;
    std::cout << "Avg time: " << avg_time << " us" << std::endl;
    std::cout << "Median time: " << median_time << " us" << std::endl;
    std::cout << "99th percentile: " << p99_time << " us" << std::endl;
    
    // 转换为更易读的单位
    std::cout << "\nHuman-readable format:" << std::endl;
    std::cout << "Avg time: " << avg_time/1000.0 << " ms" << std::endl;
    std::cout << "99th percentile: " << p99_time/1000.0 << " ms" << std::endl;
    

    // 7. 清理：删除目录（如果为空）
    try {
        if (fs::is_empty(dir)) {
            fs::remove(dir);
            std::cout << "\nCleaned up directory: " << dir << std::endl;
        }
    } catch (...) {
        // 忽略清理错误
    }

    return EXIT_SUCCESS;
}