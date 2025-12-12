import os
import random
import string
import pickle

# 配置参数
mutate_num = 99
datasets_dir = '/home/cyf/ssd1/datasets_SYN'
working_dir = '/home/cyf/ssd1/datasets_SYN_working'
average_file_size = 1024 * 1024 
init_file_num = 300

delete_percent = 0.01  # 删除的文件比例
modify_percent = 0.01  # 修改的文件比例
new_file_percent = 0.01  # 新增的文件比例

# 文件类，用来表示文件的属性
class File:
    def __init__(self, path, size, content=None):
        self.path = path  # 文件路径
        self.size = size  # 文件大小
        self.content = content  # 文件内容

    def __repr__(self):
        return f"File(path={self.path}, size={self.size})"


# 初始化文件系统
def create_initial_fs():
    fs = []
    for i in range(init_file_num):
        file_size = random.randint(1, average_file_size)  # 随机生成文件大小
        file_name = f"file{i}.txt"
        file = File(file_name, file_size)
        
        # 给文件填充随机内容
        file.content = ''.join(random.choices(string.ascii_letters + string.digits, k=file_size))  
        
        fs.append(file)
    return fs


# 保存单个文件到working_dir
def save_file(file, working_dir):
    file_path = os.path.join(working_dir, file.path)
    os.makedirs(os.path.dirname(file_path), exist_ok=True)
    with open(file_path, 'wb') as f:
        f.write(file.content.encode('utf-8'))  # 保存文件内容


# 读取文件系统
def load_fs_from_dir(working_dir):
    fs = []
    for root, dirs, files in os.walk(working_dir):
        for file_name in files:
            file_path = os.path.join(root, file_name)
            with open(file_path, 'rb') as f:
                content = f.read()
            file = File(file_name, len(content), content.decode('utf-8'))
            fs.append(file)
    return fs


# 保存整个文件系统到datasets_dir
def save_fs_to_datasets_dir(fs, datasets_dir, version):
    merged_file_path = os.path.join(datasets_dir, f"fs_version_{version}.pkl")
    with open(merged_file_path, 'wb') as f:
        pickle.dump(fs, f)
    print(f"Saved entire file system to {merged_file_path}")


# 模拟文件删除
def delete_file(fs):
    num_files_to_delete = int(len(fs) * delete_percent)
    files_to_delete = random.sample(fs, num_files_to_delete)
    for file in files_to_delete:
        fs.remove(file)
    return fs


# 模拟文件修改
def modify_file(fs):
    num_files_to_modify = int(len(fs) * modify_percent)
    files_to_modify = random.sample(fs, num_files_to_modify)
    for file in files_to_modify:
        new_size = random.randint(1, average_file_size)  # 修改为新的文件大小
        file.size = new_size
        file.content = ''.join(random.choices(string.ascii_letters + string.digits, k=new_size))  # 修改文件内容
    return fs


# 模拟文件新增
def create_new_file(fs):
    num_files_to_create = int(len(fs) * new_file_percent)
    for _ in range(num_files_to_create):
        new_file_name = f"newfile{len(fs)}.txt"
        new_file_size = random.randint(1, average_file_size)
        new_file = File(new_file_name, new_file_size)
        
        # 填充新文件内容
        new_file.content = ''.join(random.choices(string.ascii_letters + string.digits, k=new_file_size))  
        
        fs.append(new_file)
    return fs


# 主程序
if __name__ == "__main__":
    # 创建并初始化文件系统
    fs = create_initial_fs()

    # 保存初始文件系统到working_dir
    for file in fs:
        save_file(file, working_dir)

    # 在working_dir保存初始文件系统，并命名为 fs_version_0.pkl
    save_fs_to_datasets_dir(fs, datasets_dir, version=0)

    # 在文件系统上进行多次变更
    for i in range(mutate_num):
        print(f"Starting mutation {i + 1}")

        # 加载当前文件系统
        fs = load_fs_from_dir(working_dir)

        # 模拟文件系统变化
        fs = delete_file(fs)
        fs = modify_file(fs)
        fs = create_new_file(fs)

        # 保存变更后的文件系统到working_dir
        for file in fs:
            save_file(file, working_dir)

        # 保存整个文件系统到datasets_dir
        save_fs_to_datasets_dir(fs, datasets_dir, version=i + 1)
