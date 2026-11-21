// Yakuza (SLUS_213.48): support for the "Yakuza Restored 1.0" fan patch (Japanese audio, re-translated subtitles,
// revised script; an xdelta the player applies to their own US ISO, see package/LEIA-ME.txt).
//
// The patch changes data almost only: text, pointer tables, audio, movies, textures, which the port reads from the
// disc like the game does. It also changes four instructions (three in MAIN.BIN, one in OGRE4.CVM module 12); the
// recompiled code carries both versions as switched patches (switch "restored", config/ovl_main.patches.toml and
// config/ovl_ogre4_12.patches.toml). This turns the switch on when the disc's MAIN.BIN has the patched line limit
// (0x25605c sltiu ..., 4830; 4813 on the original), read straight from the ISO before the game starts.
// PS2X_RESTORED=0 / =1 forces it.
#include "game_overrides.h"
#include "ps2_runtime.h"
#include "ps2_mod_menu.h"

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>

std::atomic<bool> &ps2xSwitchFlag(const char *name);

namespace
{
    constexpr uint32_t kMainLoad = 0x00167c80u;       // MAIN.BIN's load address (config/game.env MWO3_LOAD)
    constexpr uint32_t kLineLimitPc = 0x0025605cu;     // sltiu $v0,$s0,<line count>
    constexpr uint32_t kOriginalWord = 0x2e0212cdu;    // 4813
    constexpr uint32_t kRestoredWord = 0x2e0212deu;    // 4830

    uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (uint32_t(p[3]) << 24); }

    // MAIN.BIN's first sector in a single-layer ISO9660 image (root directory only, where the game keeps it).
    bool findMainBin(std::ifstream &iso, uint32_t &lsn)
    {
        uint8_t pvd[2048];
        iso.seekg(16 * 2048);
        if (!iso.read(reinterpret_cast<char *>(pvd), sizeof(pvd)) || pvd[0] != 1 || std::memcmp(pvd + 1, "CD001", 5) != 0)
            return false;
        const uint8_t *root = pvd + 156;
        const uint32_t dirLsn = le32(root + 2), dirSize = le32(root + 10);
        std::string dir(dirSize, '\0');
        iso.seekg(uint64_t(dirLsn) * 2048);
        if (!iso.read(dir.data(), dirSize))
            return false;
        for (uint32_t pos = 0; pos < dirSize;)
        {
            const uint8_t length = static_cast<uint8_t>(dir[pos]);
            if (length == 0)
            {
                pos = (pos / 2048 + 1) * 2048; // records do not cross sectors
                continue;
            }
            const auto *record = reinterpret_cast<const uint8_t *>(dir.data() + pos);
            const std::string name(dir.data() + pos + 33, record[32]);
            if (name == "MAIN.BIN;1" || name == "MAIN.BIN")
            {
                lsn = le32(record + 2);
                return true;
            }
            pos += length;
        }
        return false;
    }

    // Options menu (F2 / L3): "Voices" English / Japanese, saved as voices= in config/mods.ini. The two are different
    // discs (the US ISO and the Restored one; the text the game loads at start differs too), so the choice applies
    // on the next start: play.sh and the packaged launcher read voices= before the game boots and pick the ISO.
    // Shown when PS2X_PATCH_AVAILABLE=1 (both discs are there; set by play.sh and the launcher).
    void addVoicesOption()
    {
        const char *available = std::getenv("PS2X_PATCH_AVAILABLE");
        if (!available || available[0] != '1')
            return;
        static std::atomic<int> wanted{ps2xSwitchFlag("restored").load() ? 1 : 0};
        ps2x_menu::Option option;
        option.key = "voices";
        option.label = "Voices";
        option.help = "English dub or Japanese (Yakuza Restored: re-translated subtitles). Applies on the next start.";
        option.choices = {"English", "Japanese"};
        option.get = []() { return wanted.load(); };
        option.set = [](int choice) { wanted.store(choice); };
        option.order = 5;
        ps2x_menu::addOption(std::move(option));
    }

    void detect()
    {
        if (std::getenv("PS2X_RESTORED"))
            return; // forced either way: ps2xSwitchFlag reads it
        std::string path = PS2Runtime::getIoPaths().cdImage.string();
        if (path.empty())
            if (const char *env = std::getenv("PS2X_CD_IMAGE"))
                path = env;
        std::ifstream iso(path, std::ios::binary);
        uint32_t lsn = 0u;
        if (!iso || !findMainBin(iso, lsn))
        {
            std::fprintf(stderr, "[restored] MAIN.BIN not found in the disc image (%s): original game assumed\n", path.c_str());
            return;
        }
        uint8_t word[4];
        iso.seekg(uint64_t(lsn) * 2048 + (kLineLimitPc - kMainLoad));
        if (!iso.read(reinterpret_cast<char *>(word), 4))
            return;
        const uint32_t value = le32(word);
        if (value == kRestoredWord)
        {
            ps2xSwitchFlag("restored").store(true);
            std::fprintf(stderr, "[restored] Yakuza Restored disc: its code changes on (switch \"restored\")\n");
        }
        else if (value != kOriginalWord)
            std::fprintf(stderr, "[restored] MAIN.BIN differs from both the original and Restored 1.0 (0x%08x): "
                                 "another patch? running it as the original\n", value);
    }
}

namespace
{
    void apply(PS2Runtime &)
    {
        detect();
        addVoicesOption(); // after detect(): with nothing saved yet the menu shows the disc that is running
    }
}

PS2_REGISTER_GAME_OVERRIDE("Yakuza Restored detection", "SLUS_213.48", 0u, 0u, apply)
