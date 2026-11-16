#include "../src/tower_bar_math.h"
#include <cmath>
#include <cstdio>
#include <limits>
static bool Near(double a, double b) { return std::abs(a - b) < 1e-9; }
int main() {
    using tower::bar::Calculate; double length = 0, fill = 0;
    if (!Calculate(4500, 5000, 850, 5500, .2, .99, &length, &fill) || !Near(fill, .9) || length < .8) return 1;
    if (!Calculate(9500, 10000, 850, 5500, .2, .99, &length, &fill) || !Near(fill, .95) || !Near(length, .99)) return 2;
    if (!Calculate(500, 10000, 850, 5500, .2, .99, &length, &fill) || !Near(fill, .05) || !Near(length, .99)) return 12;
    if (!Calculate(708.4, 1416.8, 400, 3500, .16, .92, &length, &fill) || !Near(fill,.5)) return 13;
    if (!Calculate(1416.8, 1416.8, 400, 3500, .16, .92, &length, &fill) || !Near(fill,1)) return 14;
    if (!Calculate(5000, 5000, 850, 5500, .2, .99, &length, &fill) || !Near(fill, 1)) return 3;
    if (!Calculate(0, 8000, 400, 3500, .16, .92, &length, &fill) || !Near(fill, 0) || !Near(length, .92)) return 4;
    if (Calculate(1, 0, 850, 5500, .2, .99, &length, &fill) ||
        Calculate(std::numeric_limits<double>::infinity(), 1, 850, 5500, .2, .99, &length, &fill)) return 5;
    tower::bar::LengthTransition t;
    if (t.Update(1000, .2, 1000) || !Near(t.Sample(1000), .2)) return 6;
    if (!t.Update(2000, .6, 1100) || !Near(t.Sample(1100), .2)) return 7;
    if (t.Sample(1425) <= .2 || t.Sample(1425) >= .6 || !Near(t.Sample(1750), .6)) return 8;
    if (t.Update(1000, .2, 1800) || !Near(t.Sample(1800), .2)) return 9;
    if (!t.Update(10000, .99, 1900)) return 10;
    if (!t.Update(20000, .99, 2600) || !Near(t.Sample(2600), .99)) return 11;
    // B 批：真实值来源
    float cur = 0, mx = 0;
    if (!tower::bar::Source(9500, 10000, &cur, &mx) || cur != 9500.0f || mx != 10000.0f) return 20;
    // 原生重算池属性那一帧：当前值被放大到超过塔的上限 → 夹回上限，不画超满
    if (!tower::bar::Source(12000, 10000, &cur, &mx) || cur != 10000.0f) return 21;
    if (tower::bar::Source(-1, 10000, &cur, &mx) || tower::bar::Source(100, 0, &cur, &mx) ||
        tower::bar::Source(std::numeric_limits<double>::quiet_NaN(), 100, &cur, &mx)) return 22;
    // 10000 血扣 500：填充少 1/20；上限已在原生可见上限，长度不变
    if (!Calculate(10000 - 500, 10000, 850, 5500, .2, .99, &length, &fill) || !Near(fill, .95) || !Near(length, .99)) return 23;
    // 进度条 Tick：原生没动塔写的值就不重写；任一项被原生改动、或塔还没写过就重写
    using tower::bar::NeedsReapply;
    if (NeedsReapply(.95, .99, .95, .99) || NeedsReapply(.9502, .99, .95, .99)) return 24;
    if (!NeedsReapply(.97, .99, .95, .99) || !NeedsReapply(.95, .52, .95, .99) || !NeedsReapply(.95, .99, -1, -1)) return 25;
    if (!NeedsReapply(std::numeric_limits<double>::quiet_NaN(), .99, .95, .99)) return 26;
    // 换层 / 过场后新建的控件：第一次登记直接画到目标长度，不从原生长度播增长动画（不回退、不重播）
    tower::bar::LengthTransition fresh;
    if (fresh.Update(10000, .99, 5000) || !Near(fresh.Sample(5000), .99) || !Near(fresh.Sample(5001), .99)) return 27;
    std::puts("TOWER_BAR_MATH_OK");
}
