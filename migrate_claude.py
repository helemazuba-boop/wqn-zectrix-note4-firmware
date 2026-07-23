import os
import json
import re
import shutil

# 获取当前 Linux 项目的真实绝对路径
wsl_path = os.getcwd()

# 根据 Claude Code 规则生成新的 Slug (如 /home/unknow/projects/firmware -> -home-unknow-projects-firmware)
new_slug = wsl_path.replace("/", "-")

claude_home = os.path.expanduser("~/.claude")
projects_dir = os.path.join(claude_home, "projects")
target_slug_dir = os.path.join(projects_dir, new_slug)

os.makedirs(target_slug_dir, exist_ok=True)

print(f"当前项目 WSL 路径: {wsl_path}")
print(f"目标会话 Slug 目录: {target_slug_dir}")

# 1. 自动寻找旧的 Windows 会话目录
old_slug_dirs = []
if os.path.exists(projects_dir):
    for d in os.listdir(projects_dir):
        # 匹配 Windows 盘符开头或包含关键词的旧目录
        if (d.lower().startswith("d--") or "wqn" in d.lower() or "firmware" in d.lower() or "zectrix" in d.lower()):
            old_path = os.path.join(projects_dir, d)
            if old_path != target_slug_dir and os.path.isdir(old_path):
                old_slug_dirs.append(old_path)

print(f"\n[1/4] 找到匹配的旧会话目录: {old_slug_dirs}")

if not old_slug_dirs:
    print("⚠️ 未找到匹配的旧会话目录，请检查 ~/.claude/projects/ 下的实际文件夹名称。")
else:
    # 2. 拷贝包含 Session 及其子目录的所有数据
    for old_dir in old_slug_dirs:
        for item in os.listdir(old_dir):
            src = os.path.join(old_dir, item)
            dst = os.path.join(target_slug_dir, item)
            if os.path.isdir(src):
                if os.path.exists(dst):
                    shutil.rmtree(dst)
                shutil.copytree(src, dst)
            else:
                shutil.copy2(src, dst)
    print("[2/4] 会话历史文件及关联子文件夹已成功复制。")

    # 3. 替换 .jsonl 内部所有 Windows 路径变体为当前 WSL 路径
    count = 0
    pattern = re.compile(r"[a-zA-Z]:[\\/]+[^\"]*wqn[^\"]*", re.IGNORECASE)

    for root, _, files in os.walk(target_slug_dir):
        for file in files:
            if file.endswith(".jsonl"):
                filepath = os.path.join(root, file)
                with open(filepath, "r", encoding="utf-8", errors="ignore") as f:
                    content = f.read()

                # 强力全局替换旧 Windows 路径
                new_content = pattern.sub(wsl_path, content)
                # 处理单/双转义斜杠情况
                new_content = new_content.replace("D:\\\\projects\\\\wqn-zectrix-note4-firmware", wsl_path)
                new_content = new_content.replace("D:/projects/wqn-zectrix-note4-firmware", wsl_path)
                new_content = new_content.replace("d:/projects/wqn-zectrix-note4-firmware", wsl_path)

                with open(filepath, "w", encoding="utf-8") as f:
                    f.write(new_content)
                count += 1

    print(f"[3/4] 已处理并重构 {count} 个 JSONL 会话内部路径。")

# 4. 修改全局配置文件 ~/.claude.json 中的项目注册映射
config_path = os.path.expanduser("~/.claude.json")
if os.path.exists(config_path):
    try:
        with open(config_path, "r", encoding="utf-8") as f:
            data = json.load(f)

        if "projects" in data and isinstance(data["projects"], dict):
            new_projects = {}
            for k, v in data["projects"].items():
                if "wqn" in k.lower() or "firmware" in k.lower() or k.lower().startswith("d:"):
                    new_projects[wsl_path] = v
                else:
                    new_projects[k] = v
            data["projects"] = new_projects

            with open(config_path, "w", encoding="utf-8") as f:
                json.dump(data, f, indent=2, ensure_ascii=False)
            print("[4/4] 已更新 ~/.claude.json 路径注册映射。")
    except Exception as e:
        print(f"⚠️ 更新 ~/.claude.json 过程出现非致命异常: {e}")

print("\n🎉 迁移执行成功！请在当前目录下尝试启动：")
print("   claude --resume")
