#pragma once
#include <QString>

namespace hdrshot::toolbar_style {
inline constexpr int button_size = 38;
inline constexpr int spacing = 5;
inline constexpr int horizontal_margin = 8;
inline constexpr int vertical_margin = 7;
inline constexpr int separator_width = 1;
inline constexpr int separator_height = 26;

// Shared by capture and pinned-image toolbars, including checked hover states.
inline QString sheet(bool dark) {
  return QString(
    "QFrame#editorToolbar, QFrame#propertyBar, QFrame#pinnedToolbar { background:%1; border:1px solid %3; border-radius:10px; }"
    "QToolButton { color:%2; background:transparent; border:1px solid transparent; border-radius:7px; padding:0; font-weight:600; }"
    "QToolButton:hover { background:%4; }"
    "QToolButton:pressed { background:%8; }"
    "QToolButton:checked { background:%5; color:%6; border-color:%5; }"
    "QToolButton:checked:hover { background:%9; }"
    "QToolButton:checked:pressed { background:%10; }"
    "QToolButton[primary=\"true\"] { background:#1677D2; color:white; }"
    "QToolButton[primary=\"true\"]:hover { background:#2589E8; }"
    "QToolButton[primary=\"true\"]:pressed { background:#0E5BA6; }"
    "QToolButton[analysisAction=\"true\"] { background:#174D3B; border:1px solid #347B5E; border-radius:6px; }"
    "QToolButton[analysisAction=\"true\"]:hover { background:#226448; }"
    "QToolButton[analysisAction=\"true\"]:pressed { background:#123C2E; }"
    "QToolButton[cancelAction=\"true\"] { color:%7; }"
    "QToolButton[cancelAction=\"true\"]:hover { background:%11; }"
    "QToolButton[cancelAction=\"true\"]:pressed { background:%12; }"
    "QFrame[toolbarSeparator=\"true\"] { background:%3; border:none; }"
    "QComboBox { color:%2; background:%4; border:1px solid %3; border-radius:6px; padding:0 7px; }"
    "QComboBox QAbstractItemView { color:%2; background:%1; selection-background-color:%5; }"
    "QToolTip { color:%2; background:%1; border:1px solid %3; }")
    .arg(dark?"#26292E":"#F5F6F8", dark?"#E7E9EC":"#343940",
         dark?"#44484F":"#D5D8DD", dark?"#4D525A":"#CBCED3",
         dark?"#D9D9D9":"#3B4046", dark?"#20252C":"#FFFFFF",
         dark?"#FF6B6B":"#D43D3D", dark?"#606771":"#B8BEC6",
         dark?"#EFEFEF":"#535C66", dark?"#BDBDBD":"#292F35",
         dark?"#60383E":"#F8DEDE", dark?"#803D45":"#EFBFC2");
}
} // namespace hdrshot::toolbar_style
