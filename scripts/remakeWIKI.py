with open('/home/cyf/cDedup/logs/wikiDelta.txt', 'r') as f:
    lines = f.readlines()

n = len(lines)
step = 10
group_size = n // step

result = []
for i in range(step):
    for j in range(group_size):
        idx = i + j * step
        if idx < n:
            result.append(lines[idx])

with open('/home/cyf/cDedup/logs/wikiDelta2.txt', 'w') as f:
    f.writelines(result)