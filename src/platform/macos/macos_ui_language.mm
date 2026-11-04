#include "platform/ui_language.hpp"

#import <Foundation/Foundation.h>

namespace hdrshot {

UiLanguage detect_system_ui_language() {
  @autoreleasepool {
    NSDictionary* global = [[NSUserDefaults standardUserDefaults]
        persistentDomainForName:NSGlobalDomain];
    id languages = global[@"AppleLanguages"];
    if (![languages isKindOfClass:[NSArray class]]) return UiLanguage::english;
    id first = [(NSArray*)languages firstObject];
    if (![first isKindOfClass:[NSString class]]) return UiLanguage::english;
    const char* tag = [(NSString*)first UTF8String];
    return ui_language_from_apple_language_tag(tag ? tag : "");
  }
}

}  // namespace hdrshot
