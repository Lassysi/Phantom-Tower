// 3.6.0 打包前：用真实玩家存档（参数给文件路径）核对新版能否读入；无参数时只跑内置用例
// 用法：save_upgrade_test.exe [growth.txt|run.txt ...]
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include "../src/tower_growth_run.h"
#include "../src/tower_logic.h"

static int g_fail = 0;
static std::string ReadAll(const char* p) { std::ifstream f(p, std::ios::binary); std::stringstream s; s << f.rdbuf(); return s.str(); }

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string path = argv[i], text = ReadAll(argv[i]);
        const bool growth = path.find("growth") != std::string::npos;
        bool ok = false;
        if (growth) { tower::growthrun::Runner r; ok = tower::growthrun::Runner::Load(text, &r); }
        else { tower::Run r; ok = r.Load(text); }
        std::printf("%s %s\n", ok ? "OK  " : "FAIL", argv[i]);
        if (!ok && growth) {  // 找出第一条读不进的行：逐行删掉重试
            std::istringstream in(text); std::string line; std::vector<std::string> lines;
            while (std::getline(in, line)) lines.push_back(line);
            for (size_t k = 0; k < lines.size(); ++k) {
                std::string t; for (size_t j = 0; j < lines.size(); ++j) if (j != k) t += lines[j] + "\n";
                tower::growthrun::Runner r;
                if (tower::growthrun::Runner::Load(t, &r)) { std::printf("     culprit line: %.200s\n", lines[k].c_str()); break; }
            }
        }
        g_fail += !ok;
    }
    std::printf(g_fail ? "SAVE_UPGRADE_FAILED %d\n" : "SAVE_UPGRADE_OK\n", g_fail);
    return g_fail ? 1 : 0;
}
