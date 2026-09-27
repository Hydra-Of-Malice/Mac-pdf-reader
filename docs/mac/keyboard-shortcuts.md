# Keyboard shortcuts (macOS)

Bindings follow Preview where it has an equivalent. The same list is in **Help > Keyboard Shortcuts** (press `?`);
keep it (`kShortcutRows` in `src/mac/MacPanels.mm`) and the menus in `src/mac/SumatraMac.mm` in sync with this page.

## Navigation

| Keys               | Action                                                    |
| ------------------ | --------------------------------------------------------- |
| ↓ ↑ (j k)          | scroll down / up                                          |
| Space, ⇧Space      | scroll a screen down / up                                 |
| Page Down, Page Up | scroll a screen down / up                                 |
| → ←                | next / previous page (scroll sideways when zoomed in)     |
| ⌥⌘↓ ⌥⌘↑ (n p)      | next / previous page                                      |
| ⌘↑, Home           | first page                                                |
| ⌘↓, End            | last page                                                 |
| ⌥⌘G                | go to page (focuses the page number field in the toolbar) |
| ⌘[ ⌘]              | back / forward (after links, outline, go to page, find)   |

In single page view, scrolling past the top or bottom of a page moves to the previous or next page.

## Zoom and view

| Keys                        | Action                         |
| --------------------------- | ------------------------------ |
| ⌘+ ⌘= (+)                   | zoom in                        |
| ⌘- (-)                      | zoom out                       |
| ⌘0                          | actual size                    |
| ⌘9                          | zoom to fit page               |
| ⌘8                          | zoom to fit width              |
| pinch                       | zoom around the pointer        |
| double-tap with two fingers | smart zoom in / back           |
| ⌘L ⌘R                       | rotate left / right            |
| ⌥⌘S                         | show / hide sidebar            |
| ⌥⌘3 ⌥⌘2                     | sidebar: outline / thumbnails  |
| ⌥⌘T                         | show / hide toolbar            |
| ⌃⌘F                         | enter / exit full screen       |
| ⇧⌘D                         | toggle light / dark appearance |
| ⇧⌘I                         | invert document colors         |

**View › Appearance** picks Use System Setting (default), Light or Dark. **View › Document Colors**: Match
Appearance (default; Smart Dark pages while the appearance is dark), or always Normal, Smart Dark or Inverted, and
Preserve Image Colors (Smart Dark leaves pictures as they are). Invert Colors (⇧⌘I) is for this session only, like
Shift+I in the Windows app. A **Dark Mode** toolbar button can be added with View › Customize Toolbar.

## Find and select

| Keys                  | Action                                                                  |
| --------------------- | ----------------------------------------------------------------------- |
| ⌘F                    | find: the toolbar's search field, or the find bar when it isn't visible |
| ↩ ⇧↩                  | next / previous match, in the search field                              |
| ⌘G ⇧⌘G                | find next / previous                                                    |
| ⌘E                    | use selection for find                                                  |
| ⌘C ⌘A                 | copy / select all text                                                  |
| double / triple click | select word / line; ⇧click extends                                      |
| drag outside text     | move the page                                                           |
| Esc                   | leave full screen, else clear selection/search; closes the find bar     |

## Documents and tabs

| Keys    | Action                    |
| ------- | ------------------------- |
| ⌘O      | open                      |
| ⌘W ⇧⌘W  | close tab / close window  |
| ⇧⌘T     | reopen closed tab         |
| ⌃⇥ ⌃⇧⇥  | next / previous tab       |
| ⇧⌘] ⇧⌘[ | next / previous tab       |
| ⌘D      | add / remove bookmark     |
| ⌘I      | document properties       |
| ⌘P ⇧⌘P  | print / page setup        |
| ⌘K      | command palette           |
| ?       | keyboard shortcuts window |

Open documents are also listed in the **Window** menu.
