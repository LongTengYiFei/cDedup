import redis
import secrets
import subprocess
import os
import random
import json
import time

# ============ 🎛️ 多配置定义 ============
CONFIGS = {
    "rdb": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb1",
        "key_num": 30_000,
        "NEW_RATIO": 0.01,         # 1% 新增 key
        "UPDATE_RATIO": 0.01,      # 1% 更新已存在 key 🔥
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,
        "desc": "低碎片化：1% new + 1% update per step"
    },

    "rdb2": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb2",
        "key_num": 30_000,
        "NEW_RATIO": 0.02,         # 2% 新增 key
        "UPDATE_RATIO": 0.02,      # 2% 更新已存在 key
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,
        "desc": "中碎片化：2% new + 2% update per step"
    },
    
    "rdb3": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb3_high_frag",
        "key_num": 30_000,
        "NEW_RATIO": 0.0,
        "UPDATE_RATIO": 0.30,      
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,
        "desc": "高碎片化：4% new + 4% update per step"
    },

    "rdb4": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb4",
        "key_num": 30_000,
        "NEW_RATIO": 0.0,
        "UPDATE_RATIO": 0.10,      
        "key_size": 25,
        "value_size": 50000,
        "delta_num": 99,
        "desc": "高碎片化：4% new + 4% update per step"
    },

        "rdb5": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb5",
        "key_num": 30_000,
        "NEW_RATIO": 0.0,
        "UPDATE_RATIO": 0.10,      
        "key_size": 25,
        "value_size": 5000,
        "delta_num": 99,
        "desc": "rdb5"
    },
    
    # 🔹 调试用小配置
    "tiny": {
        "rdb_dir": "/home/cyf/ssd0/MUD_MIX/rdb_tiny",
        "key_num": 1_000,
        "NEW_RATIO": 0.05,
        "UPDATE_RATIO": 0.10,
        "key_size": 16,
        "value_size": 1000,
        "delta_num": 19,
        "desc": "Debug: 1K keys, 1KB values, 20 steps"
    },
}

# ============ 🎯 手动指定当前配置 ============
# 👇 只需修改这一行，切换实验场景 👇
CONFIG_NAME = "rdb5"

# 加载配置
cfg = CONFIGS[CONFIG_NAME]
print(f"🚀 Using config: {CONFIG_NAME} — {cfg['desc']}")

# 解包配置
rdb_dir = cfg["rdb_dir"]
key_num = cfg["key_num"]
NEW_RATIO = cfg["NEW_RATIO"]
UPDATE_RATIO = cfg["UPDATE_RATIO"]
RETAIN_RATIO = 1 - NEW_RATIO - UPDATE_RATIO  # ✅ 自动计算
key_size = cfg["key_size"]
value_size = cfg["value_size"]
delta_num = cfg["delta_num"]

# 派生参数
new_keys_per_step = int(key_num * NEW_RATIO)
update_keys_per_step = int(key_num * UPDATE_RATIO)

print(f"📊 Per step: {new_keys_per_step} NEW + {update_keys_per_step} UPDATE + "
      f"{key_num - new_keys_per_step - update_keys_per_step} RETAIN")

# ============ 初始化 ============
os.makedirs(rdb_dir, exist_ok=True)
r = redis.Redis(host='127.0.0.1', port=6379, decode_responses=False)
r.config_set("appendonly", "no")
r.config_set("save", "")

print("Flushing database 0...")
r.flushdb()

meta_log = []
random.seed(42)  # 保证实验可复现

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
    "new_keys": key_num, "update_keys": 0, "retain_keys": 0,
    "NEW_RATIO": NEW_RATIO, "UPDATE_RATIO": UPDATE_RATIO, "RETAIN_RATIO": round(RETAIN_RATIO, 3),
    "rdb_path": rdb_path,
    "rdb_size": os.path.getsize(rdb_path),
    "timestamp": time.time()
})

# ============ 增量循环 ============
for i in range(1, delta_num + 1):
    print(f"Step {i}/{delta_num}: +{new_keys_per_step} NEW, +{update_keys_per_step} UPDATE")
    
    # 1️⃣ 获取当前所有 key（用于 UPDATE 选择）
    all_keys = list(r.keys())
    
    # 2️⃣ 执行 UPDATE（随机选择已存在的 key，修改其 value）
    update_keys = []
    if all_keys and update_keys_per_step > 0:
        update_keys = random.sample(all_keys, min(update_keys_per_step, len(all_keys)))
        for key in update_keys:
            # 模拟部分修改：随机改动 value 的 30%~70%（更真实）
            old_value = r.get(key)
            change_ratio = random.uniform(0.3, 0.7)
            change_start = int(len(old_value) * random.uniform(0, 1 - change_ratio))
            change_end = change_start + int(len(old_value) * change_ratio)
            new_value = (old_value[:change_start] + 
                        os.urandom(change_end - change_start) + 
                        old_value[change_end:])
            r.set(key, new_value)
        print(f"  → Updated {len(update_keys)} keys")
    
    # 3️⃣ 执行 NEW（插入全新 key）
    new_keys = []
    for _ in range(new_keys_per_step):
        key = secrets.token_hex(key_size).encode()
        value = os.urandom(value_size)
        r.set(key, value)
        new_keys.append(key)
    if new_keys:
        print(f"  → Inserted {len(new_keys)} new keys")
    
    # 4️⃣ 淘汰控制总量（如果超出 key_num，随机删除）
    current = r.dbsize()
    deleted_keys = 0
    if current > key_num:
        to_del = current - key_num
        keys = list(r.keys())
        r.delete(*random.sample(keys, to_del))
        deleted_keys = to_del
        print(f"  → Deleted {to_del} keys to maintain ~{key_num} total")
    
    # 5️⃣ 保存 RDB
    rdb_path = os.path.join(rdb_dir, f"dump_step_{i}.rdb")
    subprocess.run(["redis-cli", "--rdb", rdb_path], check=True)
    
    # 6️⃣ 记录元数据
    retain_keys = r.dbsize() - len(new_keys) - len(update_keys)
    meta_log.append({
        "step": i, "config": CONFIG_NAME,
        "total_keys": r.dbsize(),
        "new_keys": len(new_keys), 
        "update_keys": len(update_keys), 
        "retain_keys": max(0, retain_keys),
        "deleted_keys": deleted_keys,
        "NEW_RATIO": NEW_RATIO, "UPDATE_RATIO": UPDATE_RATIO, "RETAIN_RATIO": round(RETAIN_RATIO, 3),
        "rdb_path": rdb_path,
        "rdb_size": os.path.getsize(rdb_path),
        "timestamp": time.time()
    })

# ============ 收尾 ============
with open(os.path.join(rdb_dir, "meta.json"), "w") as f:
    json.dump(meta_log, f, indent=2)
print(f"✅ Done. Config={CONFIG_NAME}, meta saved to {rdb_dir}/meta.json")