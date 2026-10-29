// macOS-only "interface" submodule: adds a "Whisper AI Captions" checkbox item to VLC.app's Subtitles menu.
//
// VLC's native Cocoa menus are owned by the macosx interface module and are not extensible through any libvlc API,
// so this module is started as an extra interface (vlcrc: extraintf=vlc_whisper_menu) and inserts an NSMenuItem once
// the macosx interface has built its main menu. Toggling calls playlist_EnableAudioFilter(), which rebuilds the live
// audio filter chain, and persists the choice through the core "audio-filter" option so the next VLC launch matches.
#if defined(__APPLE__)
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <vlc_common.h>
#include <vlc_configuration.h>
#include <vlc_interface.h>
#include <vlc_playlist.h>
#include <vlc_variables.h>

#import <Cocoa/Cocoa.h>

#define VW_MENU_FILTER_NAME "vlc_whisper"
#define VW_MENU_RETRY_MAX 120  // 120 * 250 ms = 30 s to wait for the macosx interface's main menu

static bool vw_menu_filter_list_contains(const char* list, const char* name) {
  if (!list || !name) return false;
  size_t name_len = strlen(name);
  const char* p = list;
  while (*p) {
    const char* end = strchr(p, ':');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len == name_len && strncmp(p, name, len) == 0) return true;
    if (!end) break;
    p = end + 1;
  }
  return false;
}

static bool vw_menu_filter_is_enabled(intf_thread_t* intf) {
  char* list = var_InheritString(pl_Get(intf), "audio-filter");
  bool enabled = vw_menu_filter_list_contains(list, VW_MENU_FILTER_NAME);
  free(list);
  return enabled;
}

static void vw_menu_filter_set_enabled(intf_thread_t* intf, bool enable) {
  playlist_t* playlist = pl_Get(intf);
  playlist_EnableAudioFilter(playlist, VW_MENU_FILTER_NAME, enable);
  // Persist: playlist_EnableAudioFilter only updates the playlist/aout variables for this session.
  char* list = var_GetNonEmptyString(playlist, "audio-filter");
  config_PutPsz(intf, "audio-filter", list ? list : "");
  free(list);
  if (config_SaveConfigFile(intf) != 0) msg_Warn(intf, "vlc-whisper menu: could not save vlcrc");
  msg_Info(intf, "vlc-whisper menu: captions %s", enable ? "enabled" : "disabled");
}

@interface VWWhisperMenuTarget : NSObject
@property(nonatomic, assign) intf_thread_t* intf;  // cleared by Close under @synchronized(self)
@property(nonatomic, strong) NSMenuItem* item;
@property(nonatomic, strong) NSMenuItem* separator;
@property(nonatomic, assign) int retries;
@end

@implementation VWWhisperMenuTarget

- (void)toggle:(id)sender {
  (void)sender;
  @synchronized(self) {
    if (!self.intf) return;
    bool enabled = vw_menu_filter_is_enabled(self.intf);
    vw_menu_filter_set_enabled(self.intf, !enabled);
  }
}

- (BOOL)validateMenuItem:(NSMenuItem*)item {
  if (item != self.item) return YES;
  @synchronized(self) {
    if (!self.intf) return NO;
    item.state = vw_menu_filter_is_enabled(self.intf) ? NSControlStateValueOn : NSControlStateValueOff;
  }
  return YES;
}

- (NSMenu*)findSubtitlesMenu {
  NSMenu* main = [NSApp mainMenu];
  if (!main) return nil;
  NSString* localized = [NSString stringWithUTF8String:vlc_gettext("Subtitles")];
  for (NSMenuItem* top in main.itemArray) {
    if (!top.submenu) continue;
    if ([top.title isEqualToString:@"Subtitles"] || [top.title isEqualToString:localized]) return top.submenu;
  }
  return nil;
}

// Defensive re-validation: if the macosx interface ever replaces [NSApp mainMenu] or rebuilds the Subtitles menu, an
// inserted item would silently vanish. Re-check quickly during startup, then at a low rate for the session. (On VLC
// 3.0.21 a single insertion was observed to persist; this guards other versions and late menu rebuilds.)
- (void)ensureInstalled {
  @synchronized(self) {
    if (!self.intf) return;
    NSMenu* menu = [self findSubtitlesMenu];
    if (!menu) {
      if (++self.retries <= VW_MENU_RETRY_MAX) [self scheduleCheckAfterMs:250];
      else msg_Warn(self.intf, "vlc-whisper menu: Subtitles menu not found; menu toggle unavailable");
      return;
    }
    if (self.item && self.item.menu == menu) {
      [self scheduleCheckAfterMs:(self.retries < VW_MENU_RETRY_MAX ? 500 : 5000)];
      self.retries++;
      return;
    }
    if (self.item) [self detachItems];
    self.separator = [NSMenuItem separatorItem];
    self.item = [[NSMenuItem alloc] initWithTitle:@"Whisper AI Captions" action:@selector(toggle:) keyEquivalent:@""];
    self.item.target = self;
    self.item.state = vw_menu_filter_is_enabled(self.intf) ? NSControlStateValueOn : NSControlStateValueOff;
    [menu addItem:self.separator];
    [menu addItem:self.item];
    msg_Dbg(self.intf, "vlc-whisper menu: installed Subtitles menu item (menu %p)", (__bridge void*)menu);
    [self scheduleCheckAfterMs:500];
  }
}

- (void)scheduleCheckAfterMs:(int)ms {
  dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * NSEC_PER_MSEC), dispatch_get_main_queue(), ^{
    [self ensureInstalled];
  });
}

- (void)detachItems {
  NSMenuItem* item = self.item;
  NSMenuItem* separator = self.separator;
  if (item.menu) [item.menu removeItem:item];
  if (separator.menu) [separator.menu removeItem:separator];
  self.item = nil;
  self.separator = nil;
}

- (void)uninstall {
  [self detachItems];
}

@end

struct intf_sys_t {
  VWWhisperMenuTarget* target;  // ARC-retained via __strong through CFBridgingRetain/Release below
};

int vw_macos_menu_open(vlc_object_t* obj);
void vw_macos_menu_close(vlc_object_t* obj);

int vw_macos_menu_open(vlc_object_t* obj) {
  intf_thread_t* intf = (intf_thread_t*)obj;
  intf_sys_t* sys = calloc(1, sizeof(*sys));
  if (!sys) return VLC_ENOMEM;
  VWWhisperMenuTarget* target = [[VWWhisperMenuTarget alloc] init];
  target.intf = intf;
  sys->target = (__bridge VWWhisperMenuTarget*)CFBridgingRetain(target);
  intf->p_sys = sys;
  dispatch_async(dispatch_get_main_queue(), ^{
    [target ensureInstalled];
  });
  return VLC_SUCCESS;
}

void vw_macos_menu_close(vlc_object_t* obj) {
  intf_thread_t* intf = (intf_thread_t*)obj;
  intf_sys_t* sys = intf->p_sys;
  if (!sys) return;
  VWWhisperMenuTarget* target = sys->target;
  @synchronized(target) {
    target.intf = NULL;  // menu callbacks become no-ops from here on
  }
  // Menu mutation must stay on the main thread. At quit the main run loop may already be gone, so never block on it.
  if ([NSThread isMainThread]) {
    [target uninstall];
  } else {
    dispatch_async(dispatch_get_main_queue(), ^{
      [target uninstall];
    });
  }
  CFBridgingRelease((__bridge CFTypeRef)target);
  free(sys);
  intf->p_sys = NULL;
}
#endif  // __APPLE__
