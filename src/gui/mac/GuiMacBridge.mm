/* Copyright 2026 the SumatraPDF project authors (see AUTHORS file).
   License: Simplified BSD (see COPYING.BSD) */

#import <Cocoa/Cocoa.h>

#include "gui/mac/GuiMacBridge.h"

static NSString* StringFromUtf8(const char* s, int len) {
    if (!s || len <= 0) {
        return @"";
    }
    NSString* result = [[[NSString alloc] initWithBytes:s length:(NSUInteger)len
                                               encoding:NSUTF8StringEncoding] autorelease];
    return result ?: @"";
}

@interface SumatraPasswordDialogController : NSObject {
    NSTextField* _plainText;
    NSSecureTextField* _secureText;
    NSButton* _showPassword;
    NSWindow* _window;
}
- (id)initWithPlainText:(NSTextField*)plainText
             secureText:(NSSecureTextField*)secureText
           showPassword:(NSButton*)showPassword;
- (void)setWindow:(NSWindow*)window;
- (IBAction)togglePasswordVisibility:(id)sender;
- (NSString*)password;
@end

@implementation SumatraPasswordDialogController

- (id)initWithPlainText:(NSTextField*)plainText
             secureText:(NSSecureTextField*)secureText
           showPassword:(NSButton*)showPassword {
    self = [super init];
    if (self) {
        _plainText = plainText;
        _secureText = secureText;
        _showPassword = showPassword;
    }
    return self;
}

- (void)setWindow:(NSWindow*)window {
    _window = window;
}

- (IBAction)togglePasswordVisibility:(id)sender {
    (void)sender;
    bool show = [_showPassword state] == NSControlStateValueOn;
    NSTextField* from = show ? _secureText : _plainText;
    NSTextField* to = show ? _plainText : _secureText;
    [to setStringValue:[from stringValue]];
    [from setHidden:YES];
    [to setHidden:NO];
    [_window makeFirstResponder:to];
}

- (NSString*)password {
    return [_showPassword state] == NSControlStateValueOn ? [_plainText stringValue] : [_secureText stringValue];
}

@end

void MacGuiPostTask(void (*fn)(void*), void* data) {
    dispatch_async_f(dispatch_get_main_queue(), data, fn);
}

// App-modal prompt (engines ask for passwords synchronously while opening).
// isRetry: the previous password was rejected.
bool MacGuiShowPasswordDialog(void* parent, const char* fileName, int fileNameLen, bool isRetry, bool canRemember,
                              bool rememberPassword, bool showPassword, bool* rememberPasswordOut,
                              bool* showPasswordOut, char** passwordOut, int* passwordLenOut) {
    // documents may open on a loader thread; AppKit is main-thread only
    if (![NSThread isMainThread]) {
        __block bool accepted = false;
        dispatch_sync(dispatch_get_main_queue(), ^{
          accepted = MacGuiShowPasswordDialog(parent, fileName, fileNameLen, isRetry, canRemember, rememberPassword,
                                              showPassword, rememberPasswordOut, showPasswordOut, passwordOut,
                                              passwordLenOut);
        });
        return accepted;
    }
    (void)parent;
    if (passwordOut) {
        *passwordOut = nullptr;
    }
    if (passwordLenOut) {
        *passwordLenOut = 0;
    }

    NSAlert* alert = [[[NSAlert alloc] init] autorelease];
    [alert setAlertStyle:isRetry ? NSAlertStyleWarning : NSAlertStyleInformational];
    NSString* name = StringFromUtf8(fileName, fileNameLen);
    if (isRetry) {
        [alert setMessageText:@"Incorrect password"];
        [alert setInformativeText:[NSString stringWithFormat:@"The password for “%@” is incorrect. Try again.", name]];
    } else {
        [alert setMessageText:@"Password required"];
        [alert setInformativeText:[NSString stringWithFormat:@"“%@” is protected by a password.", name]];
    }
    [alert addButtonWithTitle:@"Open"];
    [alert addButtonWithTitle:@"Cancel"];

    CGFloat height = canRemember ? 92 : 66;
    NSView* accessory = [[[NSView alloc] initWithFrame:NSMakeRect(0, 0, 360, height)] autorelease];
    CGFloat textY = height - 26;
    NSSecureTextField* secureText =
        [[[NSSecureTextField alloc] initWithFrame:NSMakeRect(0, textY, 360, 24)] autorelease];
    NSTextField* plainText = [[[NSTextField alloc] initWithFrame:NSMakeRect(0, textY, 360, 24)] autorelease];
    [plainText setHidden:!showPassword];
    [secureText setHidden:showPassword];
    [accessory addSubview:secureText];
    [accessory addSubview:plainText];

    NSButton* show = [[[NSButton alloc] initWithFrame:NSMakeRect(0, textY - 26, 180, 22)] autorelease];
    [show setButtonType:NSButtonTypeSwitch];
    [show setTitle:@"Show password"];
    [show setState:showPassword ? NSControlStateValueOn : NSControlStateValueOff];
    [accessory addSubview:show];

    NSButton* remember = nil;
    if (canRemember) {
        remember = [[[NSButton alloc] initWithFrame:NSMakeRect(0, textY - 52, 340, 22)] autorelease];
        [remember setButtonType:NSButtonTypeSwitch];
        [remember setTitle:@"Remember the password for this document"];
        [remember setState:rememberPassword ? NSControlStateValueOn : NSControlStateValueOff];
        [accessory addSubview:remember];
    }

    SumatraPasswordDialogController* controller = [[SumatraPasswordDialogController alloc] initWithPlainText:plainText
                                                                                                  secureText:secureText
                                                                                                showPassword:show];
    [show setTarget:controller];
    [show setAction:@selector(togglePasswordVisibility:)];
    [secureText setPlaceholderString:@"Password"];
    [plainText setPlaceholderString:@"Password"];
    [secureText setAccessibilityLabel:@"Password"];
    [plainText setAccessibilityLabel:@"Password"];
    [alert setAccessoryView:accessory];
    [alert layout];
    [controller setWindow:[alert window]];
    [[alert window] setInitialFirstResponder:showPassword ? plainText : secureText];

    [NSApp activateIgnoringOtherApps:YES];
    NSModalResponse response = [alert runModal];
    bool accepted = response == NSAlertFirstButtonReturn;
    bool showValue = [show state] == NSControlStateValueOn;
    bool rememberValue = remember && [remember state] == NSControlStateValueOn;
    if (showPasswordOut) {
        *showPasswordOut = showValue;
    }
    if (rememberPasswordOut) {
        *rememberPasswordOut = rememberValue;
    }
    if (accepted && passwordOut && passwordLenOut) {
        NSData* utf8 = [[controller password] dataUsingEncoding:NSUTF8StringEncoding];
        int n = (int)[utf8 length];
        char* password = (char*)malloc((size_t)n + 1);
        if (password) {
            memcpy(password, [utf8 bytes], (size_t)n);
            password[n] = 0;
            *passwordOut = password;
            *passwordLenOut = n;
        } else {
            accepted = false;
        }
    }
    [controller release];
    return accepted;
}
