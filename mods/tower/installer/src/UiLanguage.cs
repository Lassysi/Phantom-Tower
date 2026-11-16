using System.Globalization;

namespace PBZTowerSetup
{
    static class UiLanguage
    {
        public static bool English = CultureInfo.CurrentUICulture.TwoLetterISOLanguageName != "zh";
        public static string T(string zh, string en) { return English ? en : zh; }
    }
}
