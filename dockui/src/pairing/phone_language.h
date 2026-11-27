#pragma once
#include <windows.h>
#include <string>
#include <map>
#include <cwchar>

namespace docky::pairing {
inline LANGID uiLanguage = GetUserDefaultUILanguage();

inline const wchar_t* Ui(const wchar_t* source) {
    const LANGID language = uiLanguage;
    static const struct { const wchar_t* key; const wchar_t* zh; const wchar_t* en; } labels[] = {
        {L"扫描球大小", L"扫描球大小", L"Bubble size"},
        {L"扫描球大小  ", L"扫描球大小  ", L"Bubble size  "},
        {L"返回水球", L"返回水球", L"Back to bubble"},
        {L"备用光纹", L"备用光纹", L"Backup pattern"},
        {L"暂时无法打开剪贴板", L"暂时无法打开剪贴板", L"Clipboard unavailable"},
        {L"文字已复制", L"文字已复制", L"Text copied"},
        {L"无法复制文字", L"无法复制文字", L"Could not copy text"},
        {L"正在接收文件…", L"正在接收文件…", L"Receiving files…"},
        {L"已接收 ", L"已接收 ", L"Received "},
        {L" 个文件", L" 个文件", L" file(s)"},
        {L"和文字", L"和文字", L" and text"},
        {L"接收完成", L"接收完成", L"Transfer complete"},
        {L"已接收文字", L"已接收文字", L"Text received"},
        {L"传输已中断", L"传输已中断", L"Transfer interrupted"},
        {L"无法接收内容", L"无法接收内容", L"Could not receive items"},
        {L"正在发送到手机…", L"正在发送到手机…", L"Sending to phone…"},
        {L"已发送到手机", L"已发送到手机", L"Sent to phone"},
        {L"发送未完成", L"发送未完成", L"Transfer incomplete"},
        {L"连接已关闭", L"连接已关闭", L"Connection closed"},
        {L"无法启动配对。请关闭后重试。", L"无法启动配对。请关闭后重试。", L"Reopen DockDrop to retry pairing."},
        {L"配对需要 Microsoft Edge WebView2 Runtime。", L"配对需要 Microsoft Edge WebView2 Runtime。", L"Pairing requires Microsoft Edge WebView2 Runtime."},
        {L"正在准备手机配对…", L"正在准备手机配对…", L"Preparing to pair…"},
        {L"关闭", L"关闭", L"Close"},
        {L"更改…", L"更改…", L"Change…"},
        {L"完成", L"完成", L"Done"},
        {L"移除…", L"移除…", L"Remove…"},
        {L"接收文件夹", L"接收資料夾", L"Destination folder"},
        {L"更改接收文件夹", L"更改接收資料夾", L"Change destination folder"},
        {L"打开接收文件夹", L"開啟接收資料夾", L"Open destination folder"},
        {L"已配对的手机", L"已配對的手機", L"Remembered phones"},
        {L"查看或移除此手机", L"查看或移除此手機", L"Review or remove this phone"},
        {L"还没有记住的手机。首次扫码后可选择记住电脑。", L"還沒有記住的手機。首次掃碼後可選擇記住電腦。", L"No remembered phones. Scan once and choose to remember this PC."},
        {L"移除此手机？下次连接需要重新扫码。", L"移除此手機？下次連線需要重新掃碼。", L"Remove this phone? It will need to scan again next time."},
        {L"无法移除手机。", L"無法移除手機。", L"Could not remove the phone."},
        {L"允许这台手机发送文件或文字？", L"允許這台手機傳送檔案或文字？", L"Allow this phone to send files or text?"},
        {L"将选中的文件或文字发送到这台手机？", L"將選取的檔案或文字傳送到這台手機？", L"Send the selected files or text to this phone?"},
        {L"选择接收文件的手机", L"選擇接收檔案的手機", L"Choose the phone to receive this file"},
        {L"扫描以连接另一台手机", L"掃描以連接另一台手機", L"Scan to connect a different phone"},
        {L"请先完成当前的 DockDrop 操作，再发送这个文件。", L"請先完成目前的 DockDrop 操作，再傳送這個檔案。", L"Finish the current DockDrop action before sending this file."},
        {L"接收设置", L"设置", L"Settings"},
        {L"电脑传输使用水球弹窗", L"電腦傳輸使用水球彈窗", L"Water-ball popup for PC transfers"},
        {L"关闭后使用通知中心样式", L"關閉後使用通知中心樣式", L"Off uses the Notification Center style"},
        {L"在后台运行", L"在背景執行", L"Run in the background"},
        {L"关闭窗口后，已配对的手机仍可发送，水球会自动弹出", L"關閉視窗後，已配對的手機仍可傳送，水球會自動彈出",
         L"Remembered phones can still send after closing"},
        {L"已开启", L"已開啟", L"On"},
        {L"已关闭", L"已關閉", L"Off"},
        {L"无法保存设置。", L"無法儲存設定。", L"Could not save this setting."},
        {L"收到电脑传输请求", L"收到電腦傳輸請求", L"Incoming PC transfer"},
        {L"想给你发送文件", L"想傳送檔案給你", L"wants to send you a file"},
        {L"请求接收文件或文字", L"請求接收檔案或文字", L"requests files or text"},
        {L"想发送文件或文字", L"想傳送檔案或文字", L"wants to send files or text"},
        {L"确认发送", L"確認傳送", L"Allow sending"},
        {L"确认接收", L"確認接收", L"Confirm"},
        {L"对方已拒绝", L"對方已拒絕", L"Transfer declined"},
        {L"等待对方确认 · ", L"等待對方確認 · ", L"Waiting for confirmation · "},
        {L"正在发送 · ", L"正在傳送 · ", L"Sending · "},
        {L"已发送 · ", L"已傳送 · ", L"Sent · "},
        {L"正在接收…", L"正在接收…", L"Receiving…"},
        {L"接受", L"接受", L"Accept"},
        {L"拒绝", L"拒絕", L"Decline"},
        {L"正在校验文件…", L"正在驗證檔案…", L"Verifying file…"},
        {L"复制文字", L"复制文字", L"Copy text"},
        {L"打开文件夹", L"打开文件夹", L"Open folder"},
        {L"选择发送文件", L"选择发送文件", L"Choose files"},
        {L"加入剪贴板文字", L"加入剪贴板文字", L"Add copied text"},
        {L"无法保存接收文件夹设置。", L"无法保存接收文件夹设置。", L"Could not save the destination folder."},
        {L"已选 ", L"已选 ", L"Selected "},
        {L"返回", L"返回", L"Back"},
        {L"返回会中断当前传输", L"返回会中断当前传输", L"Going back stops this transfer"},
        {L"未完成的文件不会保留", L"未完成的文件不会保留", L"Unfinished files are not kept"},
        {L"继续传输", L"继续传输", L"Keep going"},
        {L"中断并返回", L"中断并返回", L"Go back"},
        {L"关闭会中断当前传输", L"关闭会中断当前传输", L"Closing stops this transfer"},
        {L"中断并关闭", L"中断并关闭", L"Close"},
        {L"文件无法读取，或超过 128 MB／64 个", L"文件无法读取，或超过 128 MB／64 个", L"Unreadable files or over 128 MB / 64 files"},
        {L"文字已加入", L"文字已加入", L"Text added"},
        {L"剪贴板没有可发送的文字", L"剪贴板没有可发送的文字", L"No clipboard text to send"},
        {L"DockDrop · 配对手机", L"DockDrop · 配对手机", L"DockDrop · Pair phone"},
        {L"DockDrop · 电脑互传预览", L"DockDrop · 电脑互传预览", L"DockDrop · PC transfer preview"},
        {L"正在发现附近的电脑…", L"正在发现附近的电脑…", L"Looking for nearby PCs…"},
        {L"附近的电脑", L"附近的电脑", L"Nearby PCs"},
        {L"正在连接 · ", L"正在连接 · ", L"Connecting · "},
        {L"已连接 · ", L"已连接 · ", L"Connected · "},
        {L"取消连接", L"取消连接", L"Cancel connection"},
        {L"断开连接", L"断开连接", L"Disconnect"},
        {L"选择 DockDrop 接收文件夹", L"选择 DockDrop 接收文件夹", L"Choose DockDrop download folder"},
        {L"选择要发送到手机的文件", L"选择要发送到手机的文件", L"Choose files to send"},
        {L"正在连接配对服务…", L"正在连接配对服务…", L"Connecting…"},
        {L"正在等待手机…", L"正在等待手机…", L"Waiting for phone…"},
        {L"正在验证…", L"正在验证…", L"Verifying…"},
        {L"正在建立直连…", L"正在建立直连…", L"Connecting to phone…"},
        {L"连接中断，正在重试…", L"连接中断，正在重试…", L"Reconnecting…"},
        {L"配对服务暂时不可用，稍后自动重试…", L"配对服务暂时不可用，稍后自动重试…", L"Pairing service unavailable · retrying later…"},
    };
    for (const auto& label : labels) {
        if (std::wcscmp(source, label.key)) continue;
        if (PRIMARYLANGID(language) != LANG_CHINESE) return label.en;
        // Rows are {Simplified (the key), Traditional, English}. Many rows repeat the Simplified text in the second
        // column; those are converted to Traditional here. Rows with a hand-written Traditional text keep its wording
        // (資料夾, 傳輸...). Returning the second column for Simplified showed those rows in Traditional.
        if (SUBLANGID(language) == SUBLANG_CHINESE_SIMPLIFIED || SUBLANGID(language) == SUBLANG_CHINESE_SINGAPORE) return label.key;
        if (std::wcscmp(label.zh, label.key)) return label.zh;
        static std::map<std::wstring, std::wstring> traditional;
        auto& text = traditional[label.zh];
        if (text.empty()) {
            const int count = LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, label.zh, -1, nullptr, 0, nullptr, nullptr, 0);
            if (!count) return label.zh;
            text.resize(count);
            LCMapStringEx(L"zh-TW", LCMAP_TRADITIONAL_CHINESE, label.zh, -1, text.data(), count, nullptr, nullptr, 0);
            text.resize(count - 1);
        }
        return text.c_str();
    }
    return source;
}
}
