//-----------------------------------------------------------------------------
// 2024 Ahoy, https://ahoydtu.de
// Creative Commons - http://creativecommons.org/licenses/by-nc-sa/4.0/deed
//-----------------------------------------------------------------------------

#ifndef __LANG_H__
#define __LANG_H__

#ifdef LANG_DE
    #define REBOOT_ESP_APPLY_CHANGES "starte AhoyDTU neu, um die Änderungen zu speichern"
#else /*LANG_EN*/
    #define REBOOT_ESP_APPLY_CHANGES "reboot AhoyDTU to apply all your configuration changes"
#endif

#ifdef LANG_DE
    #define TIME_NOT_SET "keine gültige Zeit, daher keine Kommunikation zum Wechselrichter möglich"
#else /*LANG_EN*/
    #define TIME_NOT_SET "time not set. No communication to inverter possible"
#endif

#ifdef LANG_DE
    #define WAS_IN_CH_12_TO_14 "Der ESP war in WLAN Kanal 12 bis 14, was uU. zu Abstürzen führt"
#else /*LANG_EN*/
    #define WAS_IN_CH_12_TO_14 "Your ESP was in wifi channel 12 to 14. It may cause reboots of your AhoyDTU"
#endif

#ifdef LANG_DE
    #define PATH_NOT_FOUND "Pfad nicht gefunden: "
#else /*LANG_EN*/
    #define PATH_NOT_FOUND "Path not found: "
#endif

#ifdef LANG_DE
    #define INCOMPLETE_INPUT "Unvollständige Eingabe"
#else /*LANG_EN*/
    #define INCOMPLETE_INPUT "Incomplete input"
#endif

#ifdef LANG_DE
    #define INVALID_INPUT "Ungültige Eingabe"
#else /*LANG_EN*/
    #define INVALID_INPUT "Invalid input"
#endif

#ifdef LANG_DE
    #define NOT_ENOUGH_MEM "nicht genügend Speicher"
#else /*LANG_EN*/
    #define NOT_ENOUGH_MEM "Not enough memory"
#endif

#ifdef LANG_DE
    #define DESER_FAILED "Deserialisierung fehlgeschlagen"
#else /*LANG_EN*/
    #define DESER_FAILED "Deserialization failed"
#endif

#ifdef LANG_DE
    #define INV_NOT_FOUND "Wechselrichter nicht gefunden!"
#else /*LANG_EN*/
    #define INV_NOT_FOUND "inverter not found!"
#endif

#ifdef LANG_DE
    #define FACTORY_RESET "Ahoy auf Werkseinstellungen zurücksetzen"
#else /*LANG_EN*/
    #define FACTORY_RESET "Ahoy Factory Reset"
#endif

#ifdef LANG_DE
    #define BTN_REBOOT "Ahoy neustarten"
#else /*LANG_EN*/
    #define BTN_REBOOT "Reboot"
#endif

#ifdef LANG_DE
    #define BTN_REBOOT "Ahoy neustarten"
#else /*LANG_EN*/
    #define BTN_REBOOT "Reboot"
#endif

#ifdef LANG_DE
    #define BTN_YES "ja"
#else /*LANG_EN*/
    #define BTN_YES "yes"
#endif

#ifdef LANG_DE
    #define BTN_NO "nein"
#else /*LANG_EN*/
    #define BTN_NO "no"
#endif

#ifdef LANG_DE
    #define BTN_COREDUMP "CoreDump herunterladen"
#else /*LANG_EN*/
    #define BTN_COREDUMP "download CoreDump"
#endif

#endif /*__LANG_H__*/
