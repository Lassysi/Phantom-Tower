"""3.0.0 自动爬塔看守（用户 2026-11-12：后台无人值守，只在出问题时叫 Claude）。

    python -I E:\\PBZ-claude\\mods\\tower\\tools\\autoclimb_watch.py [--since-now]

一直运行，直到出现下面任一情况才退出（退出码和最后一行说明原因）：
  - autoclimb-events.log 出现 卡关 / 殒命 / 停止 / 命令被拒 / 不可用 / 未成交 / 没有选项 / 没能开下一层
  - 游戏进程消失（崩溃或被关）
  - 12 分钟没有新的一层记录（autoclimb.tsv 不增长）
  - 到达目标层（events 里出现"到达目标层"）
正常时每 30 秒检查一次，不打印任何东西。
"""
import os, re, subprocess, sys, time

TOWER = os.path.join(os.environ.get('LOCALAPPDATA', ''), 'PBZTower')
EVENTS = os.path.join(TOWER, 'autoclimb-events.log')
TSV = os.path.join(TOWER, 'autoclimb.tsv')
BAD = re.compile('卡关|殒命|停止|命令被拒|不可用|未成交|没有选项|没能开下一层|无法继续')
STALL_SECONDS = 12 * 60


def size(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def game_running():
    out = subprocess.run(['tasklist', '/FI', 'IMAGENAME eq PBZ.exe', '/NH'], capture_output=True, text=True).stdout
    return 'PBZ.exe' in out


def main():
    pos = size(EVENTS) if '--since-now' in sys.argv else 0
    rows, last_growth = size(TSV), time.time()
    seen_game = game_running()
    while True:
        time.sleep(30)
        if size(EVENTS) > pos:
            with open(EVENTS, 'rb') as f:
                f.seek(pos)
                chunk = f.read().decode('utf-8', 'replace')
            pos = size(EVENTS)
            for line in chunk.splitlines():
                if '到达目标层' in line:
                    print('DONE ' + line); return 0
                if BAD.search(line):
                    print('ANOMALY ' + line); return 2
        if size(TSV) > rows:
            rows, last_growth = size(TSV), time.time()
        elif time.time() - last_growth > STALL_SECONDS:
            print('STALL no new floor row for %d min' % (STALL_SECONDS // 60)); return 3
        running = game_running()
        if seen_game and not running:
            print('GAME_GONE PBZ.exe exited'); return 4
        seen_game = seen_game or running


if __name__ == '__main__':
    sys.exit(main())
