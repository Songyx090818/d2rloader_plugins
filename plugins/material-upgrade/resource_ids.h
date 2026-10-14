#pragma once

// RCDATA ids of the files plugin.rc embeds.
// No comments on the #define lines: rc.exe keeps a trailing comment inside the
// macro, and it would swallow the rest of the RCDATA line in plugin.rc.

// strings.json, the plugin string file
#define MATERIAL_UPGRADE_STRINGS_ID 2101

// celestialrayone.material-upgrade.toml, the default config
#define MATERIAL_UPGRADE_DEFAULT_CONFIG_ID 2102
