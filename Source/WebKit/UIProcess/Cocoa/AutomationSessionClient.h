/*
 * Copyright (C) 2016 Apple Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE INC. AND ITS CONTRIBUTORS ``AS IS''
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO,
 * THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL APPLE INC. OR ITS CONTRIBUTORS
 * BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
 * THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef AutomationSessionClient_h
#define AutomationSessionClient_h

#import "WKFoundation.h"

#if WK_API_ENABLED

#import "APIAutomationSessionClient.h"
#import "WeakObjCPtr.h"

@protocol _WKAutomationSessionDelegate;

namespace WebKit {

class AutomationSessionClient final : public API::AutomationSessionClient {
public:
    explicit AutomationSessionClient(id <_WKAutomationSessionDelegate>);

private:
    // From API::AutomationSessionClient
    WebPageProxy* didRequestNewWindow(WebAutomationSession&) override;
    void didDisconnectFromRemote(WebAutomationSession&) override;

    bool isShowingJavaScriptDialogOnPage(WebAutomationSession&, WebPageProxy&) override;
    void dismissCurrentJavaScriptDialogOnPage(WebAutomationSession&, WebPageProxy&) override;
    void acceptCurrentJavaScriptDialogOnPage(WebAutomationSession&, WebPageProxy&) override;
    String messageOfCurrentJavaScriptDialogOnPage(WebAutomationSession&, WebPageProxy&) override;
    void setUserInputForCurrentJavaScriptPromptOnPage(WebAutomationSession&, WebPageProxy&, const String&) override;
    std::optional<API::AutomationSessionClient::JavaScriptDialogType> typeOfCurrentJavaScriptDialogOnPage(WebAutomationSession&, WebPageProxy&) override;

    WeakObjCPtr<id <_WKAutomationSessionDelegate>> m_delegate;

    struct {
        bool didDisconnectFromRemote : 1;

        bool didRequestNewWebView : 1;
        bool isShowingJavaScriptDialogForWebView : 1;
        bool dismissCurrentJavaScriptDialogForWebView : 1;
        bool acceptCurrentJavaScriptDialogForWebView : 1;
        bool messageOfCurrentJavaScriptDialogForWebView : 1;
        bool setUserInputForCurrentJavaScriptPromptForWebView : 1;

        // FIXME 28524687: these delegate methods should be removed.
        bool didRequestNewWindow : 1;
        bool isShowingJavaScriptDialogOnPage : 1;
        bool dismissCurrentJavaScriptDialogOnPage : 1;
        bool acceptCurrentJavaScriptDialogOnPage : 1;
        bool messageOfCurrentJavaScriptDialogOnPage : 1;
        bool setUserInputForCurrentJavaScriptPromptOnPage : 1;
    } m_delegateMethods;
};

} // namespace WebKit

#endif

#endif // AutomationSessionClient_h
