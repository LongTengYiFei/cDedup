import redis
import secrets
import subprocess
import os
import random
import json
import time

# ============ 🎛️ 多配置定义 ============
CONFIGS = {
    # 🔹 小规模 · 快速调试（5分钟跑完）
    "tiny": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb_tiny",
        "key_num": 1_000,
        "RETAIN_RATIO": 0.9,
        "NEW_RATIO": 0.1,
        "key_size": 16,
        "value_size": 1000,      # 1KB
        "delta_num": 19,
        "desc": "Debug mode: 1K keys, 1KB values, 20 steps"
    },
    
    # 🔹 中等规模 · 当前主力实验（约1小时）
    "medium": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb",
        "key_num": 30_000,
        "RETAIN_RATIO": 0.95,
        "NEW_RATIO": 0.05,
        "key_size": 25,
        "value_size": 50000,     # 50KB
        "delta_num": 99,
        "desc": "Main experiment: 30K keys, 50KB values, 100 steps"
    },
    
    # 🔹 大规模 · 高负载测试（需大内存/磁盘）
    "large": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb_large",
        "key_num": 100_000,
        "RETAIN_RATIO": 0.98,
        "NEW_RATIO": 0.02,
        "key_size": 32,
        "value_size": 100000,    # 100KB
        "delta_num": 49,
        "desc": "Stress test: 100K keys, 100KB values, 50 steps"
    },
    
    # 🔹 高变动率 · 测试去重鲁棒性
    "high_change": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb_high_change",
        "key_num": 30_000,
        "RETAIN_RATIO": 0.7,     # 每步只保留70%，变动剧烈
        "NEW_RATIO": 0.3,        # 新增30%
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,
        "desc": "High churn: 30% new keys per step"
    },
    
    # 🔹 低变动率 · 模拟稳定业务
    "low_change": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb_low_change",
        "key_num": 30_000,
        "RETAIN_RATIO": 0.99,
        "NEW_RATIO": 0.01,       # 每步仅新增1%
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 199,        # 更多步数观察长期趋势
        "desc": "Low churn: 1% new keys per step, 200 steps"
    },

    "rdb": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb",
        "key_num": 30_000,
        "RETAIN_RATIO": 0.95,
        "NEW_RATIO": 0.05,       
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,        
        "desc": "测试rdb1"
    },

    "rdb2": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb2",
        "key_num": 30_000,
        "RETAIN_RATIO": 0.99,
        "NEW_RATIO": 0.01,       
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,        
        "desc": "测试rdb2"
    }
}

# ============ 🎯 手动指定当前配置 ============
# 👇 只需修改这一行，切换实验场景 👇
CONFIG_NAME = "rdb2"  
# 可选值: "tiny" | "medium" | "large" | "high_change" | "low_change"

# 加载配置
cfg = CONFIGS[CONFIG_NAME]
print(f"🚀 Using config: {CONFIG_NAME} — {cfg['desc']}")

# 解包配置（后续代码用局部变量，保持简洁）
rdb_dir = cfg["rdb_dir"]
key_num = cfg["key_num"]
RETAIN_RATIO = cfg["RETAIN_RATIO"]
NEW_RATIO = cfg["NEW_RATIO"]
key_size = cfg["key_size"]
value_size = cfg["value_size"]
delta_num = cfg["delta_num"]

# 派生参数
new_keys_per_step = int(key_num * NEW_RATIO)

# ============ 初始化 ============
os.makedirs(rdb_dir, exist_ok=True)
r = redis.Redis(host='127.0.0.1', port=6379, decode_responses=False)
r.config_set("appendonly", "no")
r.config_set("save", "")

print("Flushing database 0...")
r.flushdb()

meta_log = []

# ============ Warmup ============
print(f"Warmup: inserting {key_num} keys")
for _ in range(key_num):
    key = secrets.token_hex(key_size).encode()
    value = os.urandom(value_size)
    r.set(key, value)

rdb_path = os.path.join(rdb_dir, "dump_step_0.rdb")
print("Saving RDB to", rdb_path)
subprocess.run(["redis-cli", "--rdb", rdb_path], check=True)
meta_log.append({
    "step": 0, "config": CONFIG_NAME,
    "total_keys": r.dbsize(),
    "rdb_path": rdb_path,
    "rdb_size": os.path.getsize(rdb_path),
    "timestamp": time.time()
})

# ============ 增量循环 ============
for i in range(1, delta_num + 1):
    print(f"Step {i}/{delta_num}: +{new_keys_per_step} keys")
    
    # 新增
    for _ in range(new_keys_per_step):
        key = secrets.token_hex(key_size).encode()
        value = os.urandom(value_size)
        r.set(key, value)
    
    # 淘汰控制总量
    current = r.dbsize()
    if current > key_num:
        to_del = current - key_num
        keys = r.keys()
        r.delete(*random.sample(keys, to_del))
        print(f"  → Deleted {to_del} keys")
    
    # 保存 RDB
    rdb_path = os.path.join(rdb_dir, f"dump_step_{i}.rdb")
    subprocess.run(["redis-cli", "--rdb", rdb_path], check=True)
    
    # 记录元数据
    meta_log.append({
        "step": i, "config": CONFIG_NAME,
        "total_keys": r.dbsize(),
        "rdb_path": rdb_path,
        "rdb_size": os.path.getsize(rdb_path),
        "timestamp": time.time()
    })

# ============ 收尾 ============
with open(os.path.join(rdb_dir, "meta.json"), "w") as f:
    json.dump(meta_log, f, indent=2)
print(f"✅ Done. Config={CONFIG_NAME}, meta saved to {rdb_dir}/meta.json")