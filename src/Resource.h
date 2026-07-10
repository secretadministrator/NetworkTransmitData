#pragma once

// Control IDs
#define IDC_PAGE_CONTAINER     100
#define IDC_BTN_SEND           101
#define IDC_BTN_RECV           102
#define IDC_BTN_BACK           103
#define IDC_BTN_START          104
#define IDC_BTN_CANCEL         105
#define IDC_BTN_BROWSE         106
#define IDC_BTN_CONFIRM_CODE   107
#define IDC_BTN_RETRY_DISCOVER 108
#define IDC_BTN_AUTO_IP         109
#define IDC_BTN_SET_IP          110
#define IDC_BTN_RESTORE_IP      111
#define IDC_BTN_RECONNECT       112
#define IDC_COMBO_NIC           403

#define IDC_EDIT_PEER_IP        204
#define IDC_EDIT_CUSTOM_IP      205
#define IDC_EDIT_CUSTOM_MASK    206

#define IDC_EDIT_SRC_DIR       201
#define IDC_EDIT_DST_DIR       202
#define IDC_EDIT_CODE          203

#define IDC_LIST_LOG           301
#define IDC_PROGRESS           302
#define IDC_PROGRESS_TEXT      303
#define IDC_PAIRING_DISPLAY    304
#define IDC_DISCOVERY_STATUS   305
#define IDC_STAT_STATUS        306
#define IDC_STAT_SPEED         307
#define IDC_STAT_FILE          308
#define IDC_STAT_SKIP          309

#define IDC_LABEL_TITLE        400
#define IDC_LABEL_SUBTITLE     401
#define IDC_COMBO_MODE         402

// Step dot control IDs
#define IDC_STEP_DOT_1         500
#define IDC_STEP_DOT_2         501
#define IDC_STEP_DOT_3         502
#define IDC_STEP_DOT_4         503

// Custom window messages (for cross-thread UI updates)
#define WM_DISCOVERY_FOUND     (WM_USER + 100)
#define WM_DISCOVERY_FAILED    (WM_USER + 101)
#define WM_PAIRING_RESULT      (WM_USER + 102)
#define WM_TRANSFER_PROGRESS   (WM_USER + 103)
#define WM_TRANSFER_LOG        (WM_USER + 104)
#define WM_TRANSFER_DONE       (WM_USER + 105)
#define WM_STEP_CHANGED        (WM_USER + 106)

// Page container user data IDs
#define IDP_ROLE_SELECT        1000
#define IDP_SENDER             1001
#define IDP_RECEIVER           1002
