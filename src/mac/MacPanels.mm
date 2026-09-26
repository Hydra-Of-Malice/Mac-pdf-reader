/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: GPLv3 */

#import <Cocoa/Cocoa.h>

#import "mac/MacPanels.h"

// Human-readable key equivalent of a menu item, e.g. "⇧⌘G".
NSString* SumatraShortcutText(NSMenuItem* item) {
    NSString* key = [item keyEquivalent];
    if ([key length] == 0) {
        return @"";
    }
    NSEventModifierFlags mods = [item keyEquivalentModifierMask];
    unichar c = [key characterAtIndex:0];
    BOOL shift = (mods & NSEventModifierFlagShift) != 0;
    NSString* name = nil;
    switch (c) {
        case NSUpArrowFunctionKey:
            name = @"↑";
            break;
        case NSDownArrowFunctionKey:
            name = @"↓";
            break;
        case NSLeftArrowFunctionKey:
            name = @"←";
            break;
        case NSRightArrowFunctionKey:
            name = @"→";
            break;
        case '\t':
            name = @"⇥";
            break;
        case '\r':
            name = @"↩";
            break;
        case 27:
            name = @"⎋";
            break;
        case ' ':
            name = @"Space";
            break;
        case '{':
            name = @"[";
            shift = YES;
            break;
        case '}':
            name = @"]";
            shift = YES;
            break;
        default:
            if (c >= 'A' && c <= 'Z') {
                shift = YES;
            }
            name = [key uppercaseString];
            break;
    }
    NSMutableString* s = [NSMutableString string];
    if (mods & NSEventModifierFlagControl) {
        [s appendString:@"⌃"];
    }
    if (mods & NSEventModifierFlagOption) {
        [s appendString:@"⌥"];
    }
    if (shift) {
        [s appendString:@"⇧"];
    }
    if (mods & NSEventModifierFlagCommand) {
        [s appendString:@"⌘"];
    }
    [s appendString:name];
    return s;
}

#pragma mark - Command palette

// Lists the enabled commands of the main menu; typing filters (all words must
// match), ↑/↓ select, ↩ runs, Esc cancels. Runs as an app-modal panel.
@interface SumatraPaletteController
    : NSObject <NSTableViewDataSource, NSTableViewDelegate, NSTextFieldDelegate, NSWindowDelegate>
- (instancetype)initWithMenu:(NSMenu*)mainMenu;
- (NSMenuItem*)runOverWindow:(NSWindow*)parent;
@end

@implementation SumatraPaletteController {
    NSPanel* _panel;
    NSTextField* _query;
    NSTableView* _table;
    NSMutableArray* _items;
    NSMutableArray* _titles;
    NSMutableArray* _shortcuts;
    NSMutableArray* _visible;
    NSMenuItem* _chosen;
}

- (instancetype)initWithMenu:(NSMenu*)mainMenu {
    self = [super init];
    if (!self) {
        return nil;
    }
    _items = [[NSMutableArray alloc] init];
    _titles = [[NSMutableArray alloc] init];
    _shortcuts = [[NSMutableArray alloc] init];
    _visible = [[NSMutableArray alloc] init];
    for (NSMenuItem* top in [mainMenu itemArray]) {
        NSMenu* submenu = [top submenu];
        if (submenu && submenu != [NSApp servicesMenu]) {
            [self collectMenu:submenu prefix:[submenu title]];
        }
    }
    [self buildPanel];
    [self filter:@""];
    return self;
}

- (void)dealloc {
    [_panel setDelegate:nil];
    [_query setDelegate:nil];
    [_table setDataSource:nil];
    [_table setDelegate:nil];
    [_panel release];
    [_query release];
    [_table release];
    [_items release];
    [_titles release];
    [_shortcuts release];
    [_visible release];
    [_chosen release];
    [super dealloc];
}

- (void)collectMenu:(NSMenu*)menu prefix:(NSString*)prefix {
    id<NSMenuDelegate> delegate = [menu delegate];
    if ([delegate respondsToSelector:@selector(menuNeedsUpdate:)]) {
        [delegate menuNeedsUpdate:menu];
    }
    [menu update];
    for (NSMenuItem* item in [menu itemArray]) {
        if ([item isSeparatorItem] || [item isHidden] || [[item title] length] == 0) {
            continue;
        }
        NSString* title = [prefix length] ? [NSString stringWithFormat:@"%@ › %@", prefix, [item title]] : [item title];
        NSMenu* submenu = [item submenu];
        if (submenu) {
            if (submenu != [NSApp servicesMenu]) {
                [self collectMenu:submenu prefix:title];
            }
            continue;
        }
        if (![item action] || ![item isEnabled]) {
            continue;
        }
        [_items addObject:item];
        [_titles addObject:title];
        [_shortcuts addObject:SumatraShortcutText(item)];
    }
}

- (void)buildPanel {
    const CGFloat w = 560;
    const CGFloat h = 360;
    const CGFloat margin = 12;
    NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable;
    _panel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, w, h)
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
    [_panel setTitle:@"Command Palette"];
    [_panel setReleasedWhenClosed:NO];
    [_panel setDelegate:self];
    NSView* content = [_panel contentView];

    _query = [[NSTextField alloc] initWithFrame:NSMakeRect(margin, h - margin - 24, w - (2 * margin), 24)];
    [_query setPlaceholderString:@"Type a command"];
    [_query setDelegate:self];
    [_query setAccessibilityLabel:@"Command"];
    [content addSubview:_query];

    NSRect scrollFrame = NSMakeRect(margin, margin, w - (2 * margin), h - (3 * margin) - 24);
    NSScrollView* scroll = [[[NSScrollView alloc] initWithFrame:scrollFrame] autorelease];
    [scroll setHasVerticalScroller:YES];
    [scroll setBorderType:NSBezelBorder];
    _table = [[NSTableView alloc] initWithFrame:[[scroll contentView] bounds]];
    NSTableColumn* titleColumn = [[[NSTableColumn alloc] initWithIdentifier:@"title"] autorelease];
    [titleColumn setWidth:scrollFrame.size.width - 130];
    NSTableColumn* keyColumn = [[[NSTableColumn alloc] initWithIdentifier:@"key"] autorelease];
    [keyColumn setWidth:110];
    [[keyColumn dataCell] setAlignment:NSTextAlignmentRight];
    [_table addTableColumn:titleColumn];
    [_table addTableColumn:keyColumn];
    [_table setHeaderView:nil];
    [_table setDataSource:self];
    [_table setDelegate:self];
    [_table setTarget:self];
    [_table setDoubleAction:@selector(tableDoubleClicked:)];
    [_table setAccessibilityLabel:@"Commands"];
    [scroll setDocumentView:_table];
    [content addSubview:scroll];
    [_panel setInitialFirstResponder:_query];
}

- (void)filter:(NSString*)query {
    [_visible removeAllObjects];
    NSArray* words =
        [[query lowercaseString] componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceCharacterSet]];
    for (NSUInteger i = 0; i < [_titles count]; i++) {
        NSString* title = [[_titles objectAtIndex:i] lowercaseString];
        BOOL match = YES;
        for (NSString* word in words) {
            if ([word length] > 0 && [title rangeOfString:word].location == NSNotFound) {
                match = NO;
                break;
            }
        }
        if (match) {
            [_visible addObject:[NSNumber numberWithUnsignedInteger:i]];
        }
    }
    [_table reloadData];
    if ([_visible count] > 0) {
        [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:0] byExtendingSelection:NO];
        [_table scrollRowToVisible:0];
    }
}

- (void)moveSelectionBy:(NSInteger)delta {
    NSInteger count = (NSInteger)[_visible count];
    if (count == 0) {
        return;
    }
    NSInteger row = [_table selectedRow] + delta;
    row = MAX((NSInteger)0, MIN(count - 1, row));
    [_table selectRowIndexes:[NSIndexSet indexSetWithIndex:(NSUInteger)row] byExtendingSelection:NO];
    [_table scrollRowToVisible:row];
}

- (void)choose {
    NSInteger row = [_table selectedRow];
    if (row >= 0 && row < (NSInteger)[_visible count]) {
        NSUInteger idx = [[_visible objectAtIndex:(NSUInteger)row] unsignedIntegerValue];
        [_chosen release];
        _chosen = [[_items objectAtIndex:idx] retain];
    }
    [NSApp stopModal];
}

- (void)tableDoubleClicked:(id)sender {
    (void)sender;
    [self choose];
}

- (NSMenuItem*)runOverWindow:(NSWindow*)parent {
    if (parent) {
        NSRect pf = [parent frame];
        NSRect f = [_panel frame];
        [_panel setFrameOrigin:NSMakePoint(NSMidX(pf) - (f.size.width / 2.0), NSMaxY(pf) - f.size.height - 90)];
    } else {
        [_panel center];
    }
    [NSApp runModalForWindow:_panel];
    [_panel orderOut:nil];
    return [[_chosen retain] autorelease];
}

- (void)controlTextDidChange:(NSNotification*)notification {
    (void)notification;
    [self filter:[_query stringValue]];
}

- (BOOL)control:(NSControl*)control textView:(NSTextView*)textView doCommandBySelector:(SEL)command {
    (void)control;
    (void)textView;
    if (command == @selector(moveUp:)) {
        [self moveSelectionBy:-1];
        return YES;
    }
    if (command == @selector(moveDown:)) {
        [self moveSelectionBy:1];
        return YES;
    }
    if (command == @selector(insertNewline:)) {
        [self choose];
        return YES;
    }
    if (command == @selector(cancelOperation:)) {
        [NSApp stopModal];
        return YES;
    }
    return NO;
}

- (void)windowWillClose:(NSNotification*)notification {
    (void)notification;
    [NSApp stopModal];
}

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView {
    (void)tableView;
    return (NSInteger)[_visible count];
}

- (id)tableView:(NSTableView*)tableView objectValueForTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    (void)tableView;
    if (row < 0 || row >= (NSInteger)[_visible count]) {
        return @"";
    }
    NSUInteger idx = [[_visible objectAtIndex:(NSUInteger)row] unsignedIntegerValue];
    if ([[column identifier] isEqualToString:@"key"]) {
        return [_shortcuts objectAtIndex:idx];
    }
    return [_titles objectAtIndex:idx];
}

- (BOOL)tableView:(NSTableView*)tableView shouldEditTableColumn:(NSTableColumn*)column row:(NSInteger)row {
    (void)tableView;
    (void)column;
    (void)row;
    return NO;
}

@end

// Returns the chosen menu item (nil if cancelled). The caller sends its action
// after the palette is gone, so the main window's responder chain handles it.
NSMenuItem* SumatraRunCommandPalette(NSWindow* parent) {
    SumatraPaletteController* palette = [[SumatraPaletteController alloc] initWithMenu:[NSApp mainMenu]];
    NSMenuItem* item = [[[palette runOverWindow:parent] retain] autorelease];
    [palette release];
    return item;
}

#pragma mark - Keyboard shortcuts

struct ShortcutRow {
    const char* keys; // nullptr: what is a section title
    const char* what;
};

// Keep in sync with the menus in SumatraMac.mm and docs/mac/keyboard-shortcuts.md.
static const ShortcutRow kShortcutRows[] = {
    {nullptr, "Navigation"},
    {"↓  ↑  (j  k)", "Scroll down / up"},
    {"Space  ⇧Space", "Scroll a screen down / up"},
    {"Page Down  Page Up", "Scroll a screen down / up"},
    {"→  ←", "Next / previous page (scroll sideways when zoomed in)"},
    {"⌥⌘↓  ⌥⌘↑  (n  p)", "Next / previous page"},
    {"⌘↑  Home", "First page"},
    {"⌘↓  End", "Last page"},
    {"⌥⌘G", "Go to page"},
    {"⌘[  ⌘]", "Back / forward"},
    {nullptr, "Zoom and view"},
    {"⌘+  ⌘=  (+)", "Zoom in"},
    {"⌘-  (-)", "Zoom out"},
    {"⌘0", "Actual size"},
    {"⌘9", "Zoom to fit page"},
    {"⌘8", "Zoom to fit width"},
    {"Pinch", "Zoom (trackpad)"},
    {"Double-tap with two fingers", "Smart zoom (trackpad)"},
    {"⌘L  ⌘R", "Rotate left / right"},
    {"⌥⌘S", "Show / hide sidebar"},
    {"⌥⌘3  ⌥⌘2", "Sidebar: outline / thumbnails"},
    {"⌃⌘F", "Enter / exit full screen"},
    {nullptr, "Find and select"},
    {"⌘F", "Find (toolbar search field or find bar)"},
    {"⌘G  ⇧⌘G", "Find next / previous"},
    {"↩  ⇧↩", "Find next / previous (in the search field)"},
    {"⌘E", "Use selection for find"},
    {"⌘C  ⌘A", "Copy / select all text"},
    {"Double / triple click", "Select word / line"},
    {"Drag outside text", "Move the page"},
    {"Esc", "Leave full screen, clear selection and search, close the find bar"},
    {nullptr, "Documents and tabs"},
    {"⌘O", "Open"},
    {"⌘W  ⇧⌘W", "Close tab / window"},
    {"⇧⌘T", "Reopen closed tab"},
    {"⌃⇥  ⌃⇧⇥", "Next / previous tab"},
    {"⇧⌘]  ⇧⌘[", "Next / previous tab"},
    {"⌘D", "Add / remove bookmark"},
    {"⌘I", "Document properties"},
    {"⌘P  ⇧⌘P", "Print / page setup"},
    {"⌘K", "Command palette"},
    {"?", "This window"},
};

static NSAttributedString* ShortcutsText() {
    NSMutableParagraphStyle* rowStyle = [[[NSMutableParagraphStyle alloc] init] autorelease];
    NSTextTab* tab = [[[NSTextTab alloc] initWithTextAlignment:NSTextAlignmentLeft location:200
                                                       options:@{}] autorelease];
    [rowStyle setTabStops:@[ tab ]];
    [rowStyle setHeadIndent:200];
    [rowStyle setParagraphSpacing:3];
    NSMutableParagraphStyle* titleStyle = [[[NSMutableParagraphStyle alloc] init] autorelease];
    [titleStyle setParagraphSpacingBefore:10];
    [titleStyle setParagraphSpacing:4];

    NSDictionary* rowAttrs = @{
        NSFontAttributeName : [NSFont systemFontOfSize:13],
        NSForegroundColorAttributeName : [NSColor labelColor],
        NSParagraphStyleAttributeName : rowStyle,
    };
    NSDictionary* titleAttrs = @{
        NSFontAttributeName : [NSFont boldSystemFontOfSize:13],
        NSForegroundColorAttributeName : [NSColor labelColor],
        NSParagraphStyleAttributeName : titleStyle,
    };
    NSMutableAttributedString* text = [[[NSMutableAttributedString alloc] init] autorelease];
    for (const ShortcutRow& row : kShortcutRows) {
        NSString* what = [NSString stringWithUTF8String:row.what];
        NSString* line = nil;
        NSDictionary* attrs = rowAttrs;
        if (row.keys) {
            line = [NSString stringWithFormat:@"%@\t%@\n", [NSString stringWithUTF8String:row.keys], what];
        } else {
            line = [NSString stringWithFormat:@"%@\n", what];
            attrs = titleAttrs;
        }
        NSAttributedString* part = [[[NSAttributedString alloc] initWithString:line attributes:attrs] autorelease];
        [text appendAttributedString:part];
    }
    return text;
}

static NSPanel* gShortcutsPanel = nil;

void SumatraShowKeyboardShortcuts(void) {
    if (!gShortcutsPanel) {
        NSUInteger style = NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable |
                           NSWindowStyleMaskUtilityWindow;
        gShortcutsPanel = [[NSPanel alloc] initWithContentRect:NSMakeRect(0, 0, 560, 600)
                                                     styleMask:style
                                                       backing:NSBackingStoreBuffered
                                                         defer:NO];
        [gShortcutsPanel setTitle:@"Keyboard Shortcuts"];
        [gShortcutsPanel setReleasedWhenClosed:NO];
        [gShortcutsPanel setHidesOnDeactivate:YES];

        NSScrollView* scroll =
            [[[NSScrollView alloc] initWithFrame:[[gShortcutsPanel contentView] bounds]] autorelease];
        [scroll setHasVerticalScroller:YES];
        [scroll setAutoresizingMask:NSViewWidthSizable | NSViewHeightSizable];
        NSSize size = [scroll contentSize];
        NSTextView* textView =
            [[[NSTextView alloc] initWithFrame:NSMakeRect(0, 0, size.width, size.height)] autorelease];
        [textView setMinSize:NSMakeSize(0, size.height)];
        [textView setMaxSize:NSMakeSize(CGFLOAT_MAX, CGFLOAT_MAX)];
        [textView setVerticallyResizable:YES];
        [textView setHorizontallyResizable:NO];
        [textView setAutoresizingMask:NSViewWidthSizable];
        [[textView textContainer] setContainerSize:NSMakeSize(size.width, CGFLOAT_MAX)];
        [[textView textContainer] setWidthTracksTextView:YES];
        [textView setTextContainerInset:NSMakeSize(12, 12)];
        [textView setEditable:NO];
        [textView setSelectable:YES];
        [[textView textStorage] setAttributedString:ShortcutsText()];
        [textView setAccessibilityLabel:@"Keyboard shortcuts"];
        [scroll setDocumentView:textView];
        [gShortcutsPanel setContentView:scroll];
        [gShortcutsPanel center];
    }
    [gShortcutsPanel makeKeyAndOrderFront:nil];
}
