#include "lb_string_utils.h"

#include <iomanip>
#include <locale>
#include <sstream>

namespace {

std::string LbFormatBytes(double value, const char* suffix, int precision)
{
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << std::fixed << std::setprecision(precision) << value << ' ' << suffix;
    return out.str();
}

} // namespace

std::string ProjectSource_HumanBytes(unsigned long long bytes)
{
    const double value = static_cast<double>(bytes);

    if (value < 1024.0) {
        return LbFormatBytes(value, "B", 0);
    }
    if (value < 1024.0 * 1024.0) {
        return LbFormatBytes(value / 1024.0, "KB", 1);
    }
    if (value < 1024.0 * 1024.0 * 1024.0) {
        return LbFormatBytes(value / (1024.0 * 1024.0), "MB", 1);
    }
    return LbFormatBytes(value / (1024.0 * 1024.0 * 1024.0), "GB", 1);
}
