# Configuration

Open Settings with Ctrl+, or the main menu. It is a page inside the existing window. Changes save automatically; no manual reload is needed. Reset removes a GUI override so the base configuration or built-in default applies again. Reset all settings removes the browser override set.

## Files and precedence

Settings load in this order:

1. Built-in defaults.
2. `$XDG_CONFIG_HOME/noctalia-files/config.toml`, your optional base configuration.
3. `$XDG_CONFIG_HOME/noctalia-files/settings.toml`, writable GUI overrides.

When `XDG_CONFIG_HOME` is unset, use `~/.config`. Files never writes through your base configuration. This lets Nix or another declarative tool own `config.toml` while GUI changes remain writable in `settings.toml`. A base edit hidden by a GUI override becomes effective after resetting that preference.

Valid external changes apply automatically. Invalid reloads keep the last working settings and report the error. Unknown keys, incorrect types and unsupported values are errors. Files does not search `/etc/xdg` or a packaged default configuration.

This is a valid starting configuration, not a required file:

```toml
schema_version = 1

[browser]
view_mode = "list"
show_hidden = false
show_preview = true
preview_width = 320
activation_mode = "double_click"
typing_mode = "search"
sort_by = "name"
directories_first = true
```

Validate one file without opening a window or reading adjacent GUI overrides:

```sh
noctalia-files config validate /path/to/config.toml
```

`schema_version` is optional; when present it must be integer `1`.

## Browser preferences

All keys below belong in `[browser]`. Dimensions are logical pixels unless stated otherwise. View, grid zoom and sorting choices become defaults for new pages; existing pages retain their visit state. Interaction, sidebar, thumbnail and preview preferences apply across the application.

| Key | Default | Accepted values / meaning |
| --- | --- | --- |
| `view_mode` | `"list"` | `"list"`, `"grid"`, `"miller"` |
| `grid_icon_size` | `48` | `32`, `48`, `64`, `80`, `96`, `128`, `160`, `192`, `256` |
| `density` | `"compact"` | `"compact"`, `"comfortable"` |
| `show_size_caption` | `false` | Size caption in Grid view |
| `show_type_caption` | `false` | Type caption in Grid view |
| `show_modified_caption` | `false` | Modified-time caption in Grid view |
| `show_emblems` | `true` | File emblems |
| `show_hidden` | `false` | Hidden entries |
| `show_backups` | `true` | Non-directory backup names ending in `~` |
| `activation_mode` | `"double_click"` | `"single_click"`, `"double_click"` |
| `typing_mode` | `"search"` | `"search"` recursively searches filenames; `"typeahead"` selects names |
| `sort_by` | `"name"` | `"name"`, `"type"`, `"size"`, `"modified"`, `"accessed"`, `"created"`, `"owner"`, `"group"`, `"permissions"`, `"location"` |
| `sort_descending` | `false` | Reverse sort direction |
| `directories_first` | `true` | Group folders before files |
| `show_sidebar` | `true` | Show the sidebar |
| `sidebar_width` | `206` | Integer `160` through `400` |
| `sidebar_tree` | `false` | Optional sidebar folder tree |
| `show_drives` | `true` | Show drive rows |
| `expandable_folders` | `false` | Expand folders within List view |
| `show_preview` | `true` | Show the side preview when space permits |
| `preview_width` | `320` | Integer `208` through `800` |
| `miller_column_width` | `260` | Preferred column width, integer `180` through `800` |
| `miller_show_parents` | `true` | Include parent folders when entering Column view |

Individual live Miller columns can retain independent widths. The saved preferred width is a default, not a requirement for every column to occupy the same width.

### List columns

Name is always present. Type, Size and Modified are visible by default. Accessed, Created, Owner, Group, Permissions and Location are optional and hidden by default.

Visibility keys are `show_type_column`, `show_size_column`, `show_modified_column`, `show_accessed_column`, `show_created_column`, `show_owner_column`, `show_group_column`, `show_permissions_column` and `show_location_column`.

`column_order` accepts unique metadata column names. It must include `"type"`, `"size"` and `"modified"`; the optional names are `"accessed"`, `"created"`, `"owner"`, `"group"`, `"permissions"` and `"location"`. Omitted optional columns are appended in their default order. Name is not an entry in this array.

Width keys and defaults:

| Key | Default |
| --- | --- |
| `name_column_width` | `0` (automatic) |
| `type_column_width` | `152` |
| `size_column_width` | `118` |
| `modified_column_width` | `156` |
| `accessed_column_width` | `156` |
| `created_column_width` | `156` |
| `owner_column_width` | `128` |
| `group_column_width` | `128` |
| `permissions_column_width` | `112` |
| `location_column_width` | `260` |

Name accepts `0` for automatic sizing or an integer from `160` through `16384`. Other column widths accept integers from `64` through `2048`.

### Thumbnails

| Key | Default | Accepted values |
| --- | --- | --- |
| `thumbnail_policy` | `"local"` | `"never"`, `"local"`, `"all"` |
| `thumbnail_max_file_mib` | `64` | Integer `1` through `1024` |
| `thumbnail_max_megapixels` | `32` | Integer `1` through `64` |
| `thumbnail_cache_mib` | `128` | Integer `16` through `1024` |

Local policy generates thumbnails for eligible local files. All additionally permits existing cached remote thumbnails; it does not download remote files to generate previews. Provider safety limits can be narrower than a preference's maximum. Settings also provides cache inspection/clearing; Files preserves untracked shared thumbnails owned by other applications.

## Other writable data

Places state lives in `$XDG_STATE_HOME/noctalia-files/state.toml`, defaulting to `~/.local/state/noctalia-files/state.toml`. It is separate from settings and is not a complete saved window/tab session.

Thumbnails use the shared `$XDG_CACHE_HOME/thumbnails/` cache plus a Files ownership ledger under `$XDG_CACHE_HOME/noctalia-files/`. The default cache root is `~/.cache`. Custom actions live in the [Scripts directory](scripts.md).

An optional `colors.toml` beside the settings files supplies the supported Noctalia palette handoff. The installed template is `share/noctalia-files/templates/colors.toml`; broader palette/font/UI-scale preferences remain planned. See [Packaging](../../PACKAGING.md#configuration-and-writable-data) for the complete ownership and installation notes.
