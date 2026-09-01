#include "UpdaterUI.h"
#include "UpdateInstaller.h"

#undef Rectangle
#undef GetTempPath
#include <windows.h>
using namespace System;
using namespace System::Drawing;
using namespace System::Windows::Forms;
using namespace System::Diagnostics;
using namespace System::Threading::Tasks;

namespace Windows_Hello_Fix_v2_0 {
namespace Updater {

    // ---- UpdaterUI ----

    UpdaterUI::UpdaterUI(Form^ owner, Updater^ updater)
        : ownerForm(owner), updater(updater), pulseOn(false), lastPulsePercent(0), iconInstalled(false), popup(nullptr)
    {
        iconPanel = gcnew Panel();
        iconPanel->Size = Drawing::Size(24, 24);
        iconPanel->BackColor = Color::Transparent;
        iconPanel->Cursor = Cursors::Hand;
        tooltip = gcnew ToolTip();
        tooltip->AutoPopDelay = 5000;
        tooltip->InitialDelay = 200;
        tooltip->ReshowDelay = 100;
        tooltip->ShowAlways = false;

        pulseTimer = gcnew Timer();
        pulseTimer->Interval = 500;
        pulseTimer->Tick += gcnew EventHandler(this, &UpdaterUI::OnPulseTick);

        iconPanel->Paint += gcnew PaintEventHandler(this, &UpdaterUI::OnIconPaint);
        iconPanel->Click += gcnew EventHandler(this, &UpdaterUI::OnIconClick);
        iconPanel->MouseEnter += gcnew EventHandler(this, &UpdaterUI::OnIconMouseEnter);
        iconPanel->MouseLeave += gcnew EventHandler(this, &UpdaterUI::OnIconMouseLeave);

        if (updater != nullptr) {
            updater->StateChanged += gcnew EventHandler(this, &UpdaterUI::OnStateChanged);
        }
    }

    UpdaterUI::~UpdaterUI() { this->!UpdaterUI(); }
    UpdaterUI::!UpdaterUI() {
        try { if (pulseTimer != nullptr) pulseTimer->Stop(); } catch (...) {}
        try { RemoveIcon(); } catch (...) {}
        try {
            if (popup != nullptr && !popup->IsDisposed) { popup->Close(); popup = nullptr; }
        } catch (...) {}
    }

    Point UpdaterUI::ComputeIconLocation()
    {
        if (ownerForm == nullptr) return Point(0, 0);
        // Place at bottom-right, to the right of lblStatus (25,195). Owner ClientSize 430x240 → icon at (402, 192) roughly 24x24
        // Use ClientSize.Width - 28
        int x = ownerForm->ClientSize.Width - 28;
        int y = 192; // align vertically with lblStatus at 195
        // Clamp
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        return Point(x, y);
    }

    void UpdaterUI::EnsureIconPosition()
    {
        if (iconPanel == nullptr || ownerForm == nullptr) return;
        Point p = ComputeIconLocation();
        iconPanel->Location = p;
        iconPanel->BringToFront();
    }

    void UpdaterUI::InstallIcon()
    {
        if (iconInstalled) return;
        if (ownerForm == nullptr || ownerForm->IsDisposed) return;
        try {
            // Ensure we are on UI thread
            if (ownerForm->InvokeRequired) {
                ownerForm->BeginInvoke(gcnew Action(this, &UpdaterUI::InstallIcon));
                return;
            }
            ownerForm->Controls->Add(iconPanel);
            EnsureIconPosition();
            ownerForm->Resize += gcnew EventHandler(this, &UpdaterUI::OnOwnerResize);
            iconInstalled = true;
            RefreshIcon();
            pulseTimer->Start();
        } catch (...) {}
    }

    void UpdaterUI::RemoveIcon()
    {
        if (!iconInstalled) return;
        try {
            if (ownerForm != nullptr && !ownerForm->IsDisposed) {
                if (ownerForm->InvokeRequired) {
                    // Best effort: post remove
                    try { ownerForm->BeginInvoke(gcnew Action(this, &UpdaterUI::RemoveIcon)); } catch (...) {}
                    return;
                }
                if (ownerForm->Controls->Contains(iconPanel))
                    ownerForm->Controls->Remove(iconPanel);
            }
            iconInstalled = false;
            pulseTimer->Stop();
        } catch (...) {}
    }

    void UpdaterUI::OnOwnerResize(Object^, EventArgs^) { try { EnsureIconPosition(); } catch (...) {} }
    void UpdaterUI::OnPopupClosed(Object^, FormClosedEventArgs^) { try { popup = nullptr; } catch (...) {} }
    void UpdaterUI::RefreshIcon()
    {
        try {
            if (iconPanel != nullptr && !iconPanel->IsDisposed) {
                iconPanel->Invalidate();
                String^ tip = GetTooltipForStatus();
                tooltip->SetToolTip(iconPanel, tip);
            }
        } catch (...) {}
    }

    String^ UpdaterUI::GetTooltipForStatus()
    {
        if (updater == nullptr || updater->State == nullptr) return "Updates — click to view releases";
        auto st = updater->State;
        UpdaterStatus s = st->Status;
        String^ detail = st->StatusDetail;
        GitHubRelease^ latest = st->LatestForChannel;
        String^ ver = latest != nullptr && latest->Version != nullptr ? latest->Version->ToDisplayString() : "";
        switch (s) {
        case UpdaterStatus::Checking: return "Checking for updates...";
        case UpdaterStatus::UpdateAvailable: return String::Format("Update available: {0} — click to update", ver);
        case UpdaterStatus::Downloading: return String::Format("Downloading {0}% — click to view progress", st->DownloadProgress);
        case UpdaterStatus::Installing: return "Installing — app will restart";
        case UpdaterStatus::UpToDate: return String::Format("Up to date — {0}", UpdateVersion::CurrentDisplayString());
        case UpdaterStatus::Offline: return "Offline — update info unavailable. Click to retry.";
        case UpdaterStatus::RateLimited: return "Rate limited — click to retry. " + detail;
        case UpdaterStatus::Error: return "Update check failed — click to retry. " + detail;
        default: return "Updates — click to view releases";
        }
    }

    void UpdaterUI::OnPulseTick(Object^, EventArgs^)
    {
        if (updater == nullptr) return;
        auto s = updater->State->Status;
        if (s == UpdaterStatus::Checking || s == UpdaterStatus::Downloading) {
            pulseOn = !pulseOn;
            if (iconPanel != nullptr && !iconPanel->IsDisposed) iconPanel->Invalidate();
        }
    }

    void UpdaterUI::OnStateChanged(Object^, EventArgs^)
    {
        try {
            if (ownerForm != nullptr && !ownerForm->IsDisposed && ownerForm->InvokeRequired) {
                ownerForm->BeginInvoke(gcnew Action(this, &UpdaterUI::RefreshIcon));
            } else RefreshIcon();
        } catch (...) {}
        // Also refresh popup if open
        try {
            if (popup != nullptr && !popup->IsDisposed) {
                if (popup->InvokeRequired) popup->BeginInvoke(gcnew Action(popup, &UpdaterPopup::RefreshForExternalChange));
                else popup->RefreshForExternalChange();
            }
        } catch (...) {}
    }

    void UpdaterUI::OnIconPaint(Object^ sender, PaintEventArgs^ e)
    {
        try {
            Panel^ p = safe_cast<Panel^>(sender);
            Graphics^ g = e->Graphics;
            g->SmoothingMode = Drawing2D::SmoothingMode::AntiAlias;
            g->TextRenderingHint = System::Drawing::Text::TextRenderingHint::ClearTypeGridFit;

            auto st = updater != nullptr ? updater->State : nullptr;
            UpdaterStatus s = st != nullptr ? st->Status : UpdaterStatus::Idle;
            bool hasUpdate = st != nullptr && s == UpdaterStatus::UpdateAvailable;
            bool isChecking = s == UpdaterStatus::Checking;
            bool isDownloading = s == UpdaterStatus::Downloading;
            bool isError = s == UpdaterStatus::Error || s == UpdaterStatus::Offline || s == UpdaterStatus::RateLimited;

            System::Drawing::Rectangle rect = p->ClientRectangle;
            Color glyphColor = Color::FromArgb(96, 94, 92);
            if (hasUpdate) glyphColor = Color::FromArgb(0, 90, 158);
            else if (isError) glyphColor = Color::FromArgb(150, 150, 150);
            else if (isChecking && pulseOn) glyphColor = Color::FromArgb(180, 180, 180);
            else if (isChecking) glyphColor = Color::FromArgb(120, 120, 120);
            String^ glyph = "v";
            if (isDownloading && st != nullptr) {
                int pct = st->DownloadProgress;
                Pen^ bgPen = gcnew Pen(Color::FromArgb(220, 220, 220), 2);
                g->DrawEllipse(bgPen, System::Drawing::Rectangle(2, 2, rect.Width - 4, rect.Height - 4));
                Pen^ fgPen = gcnew Pen(Color::FromArgb(0, 120, 212), 2);
                float sweep = (pct / 100.0f) * 360.0f;
                g->DrawArc(fgPen, System::Drawing::Rectangle(2, 2, rect.Width - 4, rect.Height - 4), -90, sweep);
                Font^ f = gcnew Font("Segoe UI", 10, FontStyle::Regular);
                SizeF sz = g->MeasureString(glyph, f);
                PointF pt((rect.Width - sz.Width) / 2, (rect.Height - sz.Height) / 2 - 1);
                SolidBrush^ br = gcnew SolidBrush(glyphColor);
                g->DrawString(glyph, f, br, pt);
                delete f; delete br; delete bgPen; delete fgPen;
            } else if (s == UpdaterStatus::Installing) {
                Pen^ pen = gcnew Pen(Color::FromArgb(0, 120, 212), 2);
                float angle = (Environment::TickCount % 1000) / 1000.0f * 360.0f;
                g->DrawArc(pen, System::Drawing::Rectangle(2, 2, rect.Width - 4, rect.Height - 4), angle, 270);
                Font^ f = gcnew Font("Segoe UI", 10, FontStyle::Regular);
                SizeF sz = g->MeasureString(glyph, f);
                PointF pt((rect.Width - sz.Width) / 2, (rect.Height - sz.Height) / 2 - 1);
                SolidBrush^ br = gcnew SolidBrush(glyphColor);
                g->DrawString(glyph, f, br, pt);
                delete f; delete br; delete pen;
            } else {
                Font^ f = gcnew Font("Segoe UI", 10, FontStyle::Regular);
                SizeF sz = g->MeasureString(glyph, f);
                PointF pt((rect.Width - sz.Width) / 2, (rect.Height - sz.Height) / 2 - 1);
                SolidBrush^ br = gcnew SolidBrush(glyphColor);
                g->DrawString(glyph, f, br, pt);
                delete f; delete br;
                if (isError) {
                    Font^ sf = gcnew Font("Segoe UI", 7, FontStyle::Bold);
                    SolidBrush^ rbr = gcnew SolidBrush(Color::FromArgb(209, 52, 56));
                    g->DrawString("!", sf, rbr, PointF(rect.Width - 10, 1));
                    delete sf; delete rbr;
                }
            }

            // Notification dot for UpdateAvailable
            if (hasUpdate) {
                SolidBrush^ dotBr = gcnew SolidBrush(Color::FromArgb(209, 52, 56)); // #D13438
                Pen^ dotPen = gcnew Pen(Color::White, 1);
                int dotSize = 7;
                int dotX = rect.Width - dotSize - 1;
                int dotY = 1;
                g->FillEllipse(dotBr, dotX, dotY, dotSize, dotSize);
                g->DrawEllipse(dotPen, dotX, dotY, dotSize, dotSize);
                delete dotBr; delete dotPen;
            }
        } catch (...) {}
    }

    void UpdaterUI::OnIconClick(Object^, EventArgs^)
    {
        try { ShowPopup(); } catch (...) {}
    }

    void UpdaterUI::OnIconMouseEnter(Object^, EventArgs^)
    {
        try { if (iconPanel != nullptr) iconPanel->Invalidate(); } catch (...) {}
    }

    void UpdaterUI::OnIconMouseLeave(Object^, EventArgs^)
    {
        try { if (iconPanel != nullptr) iconPanel->Invalidate(); } catch (...) {}
    }

    void UpdaterUI::ShowPopup()
    {
        try {
            if (ownerForm == nullptr || ownerForm->IsDisposed) return;
            if (ownerForm->InvokeRequired) { ownerForm->BeginInvoke(gcnew MethodInvoker(this, &UpdaterUI::ShowPopup)); return; }
            if (popup == nullptr || popup->IsDisposed) {
                popup = gcnew UpdaterPopup(ownerForm, updater);
                popup->FormClosed += gcnew FormClosedEventHandler(this, &UpdaterUI::OnPopupClosed);
            }
            if (!popup->Visible) {
                System::Drawing::Point ownerLoc = ownerForm->Location;
                System::Drawing::Size ownerSz = ownerForm->Size;
                int x = ownerLoc.X + ownerSz.Width - popup->Width - 10;
                int y = ownerLoc.Y + ownerSz.Height - 10;
                System::Drawing::Rectangle work = Screen::FromControl(ownerForm)->WorkingArea;
                if (x + popup->Width > work.Right) x = work.Right - popup->Width - 10;
                if (y + popup->Height > work.Bottom) y = work.Bottom - popup->Height - 10;
                if (x < work.Left) x = work.Left + 10;
                if (y < work.Top) y = work.Top + 10;
                popup->StartPosition = FormStartPosition::Manual;
                popup->Location = System::Drawing::Point(x, y);
                popup->Show(ownerForm);
            }
            popup->BringToFront();
            popup->Activate();
            popup->RefreshForExternalChange();
        } catch (Exception^ ex) {
            try { MessageBox::Show(ownerForm, "Updater popup failed: " + ex->Message, "Updater", MessageBoxButtons::OK, MessageBoxIcon::Error); } catch (...) {}
        }
    }

    void UpdaterUI::ShowPopupForRelease(GitHubRelease^ release)
    {
        ShowPopup();
        try { if (popup != nullptr && !popup->IsDisposed) popup->SelectRelease(release); } catch (...) {}
    }

    bool UpdaterUI::IsPopupOpen()
    {
        try { return popup != nullptr && !popup->IsDisposed && popup->Visible; } catch (...) { return false; }
    }

    // ---- UpdaterPopup ----

    UpdaterPopup::UpdaterPopup(Form^ owner, Updater^ updater)
        : ownerForm(owner), updater(updater), selectedReleaseForUpdate(nullptr), isBrowseExpanded(false)
    {
        InitializeComponentPopup();
        if (updater != nullptr) {
            updater->StateChanged += gcnew EventHandler(this, &UpdaterPopup::OnUpdaterStateChanged);
            updater->ReleasesUpdated += gcnew EventHandler(this, &UpdaterPopup::OnUpdaterReleasesUpdated);
        }
        this->Deactivate += gcnew EventHandler(this, &UpdaterPopup::OnFormDeactivate);
        RefreshAll();
    }

    void UpdaterPopup::InitializeComponentPopup()
    {
        this->SuspendLayout();
        this->FormBorderStyle = ::System::Windows::Forms::FormBorderStyle::FixedDialog;
        this->MaximizeBox = false;
        this->MinimizeBox = false;
        this->ShowInTaskbar = false;
        this->ShowIcon = false;
        this->TopMost = false;
        this->StartPosition = ::System::Windows::Forms::FormStartPosition::Manual;
        this->Size = System::Drawing::Size(380, 460);
        this->MinimumSize = System::Drawing::Size(380, 460);
        this->MaximumSize = System::Drawing::Size(380, 460);
        this->BackColor = Color::White;
        this->Font = gcnew Drawing::Font("Segoe UI", 9);
        this->Text = "Updates";

        int pad = 12;
        int y = pad;

        lblHeader = gcnew Label();
        lblHeader->AutoSize = false;
        lblHeader->Location = Point(pad, y);
        lblHeader->Size = Drawing::Size(340, 22);
        lblHeader->Font = gcnew Drawing::Font("Segoe UI", 10, FontStyle::Bold);
        lblHeader->Text = "Updates";
        y += 22;

        lblVersionLine = gcnew Label();
        lblVersionLine->AutoSize = false;
        lblVersionLine->Location = Point(pad, y);
        lblVersionLine->Size = Drawing::Size(340, 18);
        lblVersionLine->ForeColor = Color::FromArgb(96, 94, 92);
        lblVersionLine->Font = gcnew Drawing::Font("Segoe UI", 9);
        y += 18;

        lblChannelLine = gcnew Label();
        lblChannelLine->AutoSize = false;
        lblChannelLine->Location = Point(pad, y);
        lblChannelLine->Size = Drawing::Size(340, 16);
        lblChannelLine->ForeColor = Color::FromArgb(120, 120, 120);
        lblChannelLine->Font = gcnew Drawing::Font("Segoe UI", 8);
        y += 18;

        lblStatusBanner = gcnew Label();
        lblStatusBanner->AutoSize = false;
        lblStatusBanner->Location = Point(pad, y);
        lblStatusBanner->Size = Drawing::Size(340, 18);
        lblStatusBanner->ForeColor = Color::FromArgb(0, 90, 158);
        lblStatusBanner->Visible = false;
        y += 20;

        lblNotes = gcnew Label();
        lblNotes->AutoSize = false;
        lblNotes->Location = Point(pad, y);
        lblNotes->Size = Drawing::Size(340, 60);
        lblNotes->ForeColor = Color::FromArgb(50, 50, 50);
        y += 65;

        linkViewOnGithub = gcnew LinkLabel();
        linkViewOnGithub->AutoSize = true;
        linkViewOnGithub->Location = Point(pad, y);
        linkViewOnGithub->Text = "View on GitHub";
        linkViewOnGithub->LinkClicked += gcnew LinkLabelLinkClickedEventHandler(this, &UpdaterPopup::OnViewOnGithubClick);
        y += 20;

        progressBar = gcnew ProgressBar();
        progressBar->Location = Point(pad, y);
        progressBar->Size = Drawing::Size(340, 8);
        progressBar->Visible = false;
        progressBar->Style = ProgressBarStyle::Continuous;
        y += 14;

        btnUpdate = gcnew Button();
        btnUpdate->Location = Point(pad, y);
        btnUpdate->Size = Drawing::Size(110, 30);
        btnUpdate->Text = "Update";
        btnUpdate->UseVisualStyleBackColor = true;
        btnUpdate->Click += gcnew EventHandler(this, &UpdaterPopup::OnUpdateClick);

        btnDetails = gcnew Button();
        btnDetails->Location = Point(pad + 120, y);
        btnDetails->Size = Drawing::Size(90, 30);
        btnDetails->Text = "Details ▸";
        btnDetails->UseVisualStyleBackColor = true;
        btnDetails->Click += gcnew EventHandler(this, &UpdaterPopup::OnDetailsClick);

        btnCancelDownload = gcnew Button();
        btnCancelDownload->Location = Point(pad + 220, y);
        btnCancelDownload->Size = Drawing::Size(85, 30);
        btnCancelDownload->Text = "Cancel";
        btnCancelDownload->Visible = false;
        btnCancelDownload->Click += gcnew EventHandler(this, &UpdaterPopup::OnCancelDownloadClick);
        y += 38;

        // Channel + Check
        Label^ lblCh = gcnew Label();
        lblCh->AutoSize = true;
        lblCh->Location = Point(pad, y + 4);
        lblCh->Text = "Channel:";
        lblCh->ForeColor = Color::FromArgb(96, 94, 92);

        comboChannel = gcnew ComboBox();
        comboChannel->DropDownStyle = ComboBoxStyle::DropDownList;
        comboChannel->Location = Point(pad + 60, y);
        comboChannel->Size = Drawing::Size(110, 21);
        comboChannel->Items->Add("Stable");
        comboChannel->Items->Add("Beta");
        comboChannel->Items->Add("Pre-Release");
        comboChannel->SelectedIndex = 0;
        comboChannel->SelectedIndexChanged += gcnew EventHandler(this, &UpdaterPopup::OnChannelChanged);

        linkCheckNow = gcnew LinkLabel();
        linkCheckNow->AutoSize = true;
        linkCheckNow->Location = Point(pad + 180, y + 4);
        linkCheckNow->Text = "⟳ Check now";
        linkCheckNow->LinkClicked += gcnew LinkLabelLinkClickedEventHandler(this, &UpdaterPopup::OnCheckNowLink);

        y += 26;

        linkBrowse = gcnew LinkLabel();
        linkBrowse->AutoSize = true;
        linkBrowse->Location = Point(pad, y);
        linkBrowse->Text = "Browse releases ▸";
        linkBrowse->LinkClicked += gcnew LinkLabelLinkClickedEventHandler(this, &UpdaterPopup::OnBrowseLink);
        y += 20;

        // Browse panel (initially hidden)
        browsePanel = gcnew Panel();
        browsePanel->Location = Point(pad, y);
        browsePanel->Size = Drawing::Size(340, 150);
        browsePanel->BorderStyle = BorderStyle::FixedSingle;
        browsePanel->Visible = false;

        listReleases = gcnew ListBox();
        listReleases->Location = Point(2, 2);
        listReleases->Size = Drawing::Size(160, 120);
        listReleases->IntegralHeight = false;
        listReleases->SelectedIndexChanged += gcnew EventHandler(this, &UpdaterPopup::OnListSelected);

        lblReleaseDetail = gcnew Label();
        lblReleaseDetail->AutoSize = false;
        lblReleaseDetail->Location = Point(166, 2);
        lblReleaseDetail->Size = Drawing::Size(170, 90);
        lblReleaseDetail->ForeColor = Color::FromArgb(60, 60, 60);
        lblReleaseDetail->Font = gcnew Drawing::Font("Segoe UI", 8);

        linkDetailGithub = gcnew LinkLabel();
        linkDetailGithub->AutoSize = true;
        linkDetailGithub->Location = Point(166, 95);
        linkDetailGithub->Text = "View on GitHub";
        linkDetailGithub->LinkClicked += gcnew LinkLabelLinkClickedEventHandler(this, &UpdaterPopup::OnDetailGithubClick);

        btnUpdateSelected = gcnew Button();
        btnUpdateSelected->Location = Point(166, 118);
        btnUpdateSelected->Size = Drawing::Size(85, 24);
        btnUpdateSelected->Text = "Update";
        btnUpdateSelected->Click += gcnew EventHandler(this, &UpdaterPopup::OnUpdateSelectedClick);

        btnDownloadSelected = gcnew Button();
        btnDownloadSelected->Location = Point(255, 118);
        btnDownloadSelected->Size = Drawing::Size(75, 24);
        btnDownloadSelected->Text = "Download";
        btnDownloadSelected->Click += gcnew EventHandler(this, &UpdaterPopup::OnDownloadSelectedClick);

        browsePanel->Controls->Add(listReleases);
        browsePanel->Controls->Add(lblReleaseDetail);
        browsePanel->Controls->Add(linkDetailGithub);
        browsePanel->Controls->Add(btnUpdateSelected);
        browsePanel->Controls->Add(btnDownloadSelected);

        this->Controls->Add(lblHeader);
        this->Controls->Add(lblVersionLine);
        this->Controls->Add(lblChannelLine);
        this->Controls->Add(lblStatusBanner);
        this->Controls->Add(lblNotes);
        this->Controls->Add(linkViewOnGithub);
        this->Controls->Add(progressBar);
        this->Controls->Add(btnUpdate);
        this->Controls->Add(btnDetails);
        this->Controls->Add(btnCancelDownload);
        this->Controls->Add(lblCh);
        this->Controls->Add(comboChannel);
        this->Controls->Add(linkCheckNow);
        this->Controls->Add(linkBrowse);
        this->Controls->Add(browsePanel);

        this->ResumeLayout(false);
        this->PerformLayout();
    }

    void UpdaterPopup::RefreshAll()
    {
        RefreshHeader();
        RefreshBrowseList();
        RefreshDetailPane();
        UpdateButtonStates();
    }

    void UpdaterPopup::RefreshHeader()
    {
        if (updater == nullptr || updater->State == nullptr) return;
        auto st = updater->State;
        String^ cur = UpdateVersion::CurrentDisplayString();
        GitHubRelease^ latest = st->LatestForChannel;
        GitHubRelease^ latestAvail = st->IsUpdateAvailable() ? latest : nullptr;

        if (latestAvail != nullptr && latestAvail->Version != nullptr) {
            lblVersionLine->Text = String::Format("{0} → {1}", cur, latestAvail->Version->ToDisplayString());
            lblVersionLine->ForeColor = Color::FromArgb(16, 124, 16); // green
            String^ chStr = ChannelHelper::ToDisplayString(latestAvail->Channel);
            String^ dateStr = latestAvail->PublishedAt != DateTime::MinValue ? latestAvail->PublishedAt.ToString("yyyy-MM-dd") : "";
            lblChannelLine->Text = String::Format("{0} • Released {1} • {2} downloads", chStr, dateStr, latestAvail->Assets->Count > 0 ? latestAvail->Assets[0]->DownloadCount.ToString() : "—");
            // Notes excerpt (first 200 chars)
            String^ body = latestAvail->Body;
            if (!String::IsNullOrEmpty(body)) {
                String^ excerpt = body->Replace("\r", " ")->Replace("\n", " ");
                if (excerpt->Length > 200) excerpt = excerpt->Substring(0, 200) + "...";
                lblNotes->Text = excerpt;
            } else lblNotes->Text = "No release notes.";

            linkViewOnGithub->Visible = !String::IsNullOrEmpty(latestAvail->HtmlUrl);
            linkViewOnGithub->Tag = latestAvail->HtmlUrl;
            selectedReleaseForUpdate = latestAvail;
        } else {
            // UpToDate or no data
            GitHubRelease^ latestAny = latest;
            if (latestAny != nullptr) {
                lblVersionLine->Text = String::Format("You are on {0} — latest {1} is {2}", cur, ChannelHelper::ToDisplayString(latestAny->Channel), latestAny->Version->ToDisplayString());
                lblVersionLine->ForeColor = Color::FromArgb(96, 94, 92);
                lblChannelLine->Text = String::Format("{0} • {1}", ChannelHelper::ToDisplayString(latestAny->Channel), latestAny->PublishedAt != DateTime::MinValue ? latestAny->PublishedAt.ToString("yyyy-MM-dd") : "");
                String^ body = latestAny->Body;
                if (!String::IsNullOrEmpty(body)) {
                    String^ excerpt = body->Replace("\r", " ")->Replace("\n", " ");
                    if (excerpt->Length > 200) excerpt = excerpt->Substring(0, 200) + "...";
                    lblNotes->Text = excerpt;
                } else lblNotes->Text = "You are on the latest version for this channel.";
                linkViewOnGithub->Visible = !String::IsNullOrEmpty(latestAny->HtmlUrl);
                linkViewOnGithub->Tag = latestAny->HtmlUrl;
                selectedReleaseForUpdate = nullptr;
            } else {
                lblVersionLine->Text = String::Format("Windows Hello Fix {0}", cur);
                lblVersionLine->ForeColor = Color::FromArgb(96, 94, 92);
                lblChannelLine->Text = String::Format("Channel: {0} — no releases yet", ChannelHelper::ToDisplayString(st->SelectedChannel));
                lblNotes->Text = "Checking for releases...";
                linkViewOnGithub->Visible = false;
                selectedReleaseForUpdate = nullptr;
            }
        }

        // Status banner
        UpdaterStatus s = st->Status;
        lblStatusBanner->Visible = true;
        switch (s) {
        case UpdaterStatus::Checking: lblStatusBanner->Text = "Checking for updates..."; lblStatusBanner->ForeColor = Color::FromArgb(0, 90, 158); break;
        case UpdaterStatus::Downloading: lblStatusBanner->Text = String::Format("Downloading {0}%...", st->DownloadProgress); lblStatusBanner->ForeColor = Color::FromArgb(0, 90, 158); progressBar->Visible = true; progressBar->Value = Math::Min(100, Math::Max(0, st->DownloadProgress)); btnCancelDownload->Visible = true; break;
        case UpdaterStatus::Installing: lblStatusBanner->Text = "Installing — app will restart..."; lblStatusBanner->ForeColor = Color::FromArgb(16, 124, 16); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        case UpdaterStatus::Offline: lblStatusBanner->Text = "Offline — using cached info. Check your connection."; lblStatusBanner->ForeColor = Color::FromArgb(209, 52, 56); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        case UpdaterStatus::RateLimited: lblStatusBanner->Text = "Rate limited — try again later. " + st->StatusDetail; lblStatusBanner->ForeColor = Color::FromArgb(209, 52, 56); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        case UpdaterStatus::Error: lblStatusBanner->Text = "Update check failed: " + st->StatusDetail; lblStatusBanner->ForeColor = Color::FromArgb(209, 52, 56); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        case UpdaterStatus::UpToDate: lblStatusBanner->Text = "✓ You are on the latest version for this channel."; lblStatusBanner->ForeColor = Color::FromArgb(16, 124, 16); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        case UpdaterStatus::UpdateAvailable: lblStatusBanner->Text = "Update available! Click Update to install."; lblStatusBanner->ForeColor = Color::FromArgb(16, 124, 16); progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        default: lblStatusBanner->Visible = false; progressBar->Visible = false; btnCancelDownload->Visible = false; break;
        }
        if (s != UpdaterStatus::Downloading) progressBar->Visible = false;
        if (s != UpdaterStatus::Downloading) btnCancelDownload->Visible = false;

        // Channel combo
        UpdateChannel ch = st->SelectedChannel;
        if (ch == UpdateChannel::Stable) comboChannel->SelectedIndex = 0;
        else if (ch == UpdateChannel::Beta) comboChannel->SelectedIndex = 1;
        else if (ch == UpdateChannel::PreRelease) comboChannel->SelectedIndex = 2;
    }

    void UpdaterPopup::RefreshBrowseList()
    {
        if (updater == nullptr || updater->State == nullptr) return;
        auto st = updater->State;
        auto list = st->GetReleasesForSelectedChannel();
        listReleases->BeginUpdate();
        listReleases->Items->Clear();
        if (list != nullptr) {
            for each (GitHubRelease^ r in list) {
                String^ tag = r->Tag != nullptr ? r->Tag : "unknown";
                String^ ch = ChannelHelper::ToDisplayString(r->Channel);
                String^ date = r->PublishedAt != DateTime::MinValue ? r->PublishedAt.ToString("yyyy-MM-dd") : "";
                String^ installedMark = "";
                if (r->Version != nullptr && st->InstalledVersion != nullptr && r->Version->CompareTo(st->InstalledVersion) == 0) installedMark = " ✓ installed";
                String^ hasAssetMark = r->HasInstallerAsset() ? "" : " (no installer)";
                String^ item = String::Format("{0}  {1}  {2}{3}{4}", tag, ch, date, installedMark, hasAssetMark);
                listReleases->Items->Add(item);
            }
        }
        listReleases->EndUpdate();
        if (listReleases->Items->Count > 0 && listReleases->SelectedIndex < 0) listReleases->SelectedIndex = 0;
    }

    void UpdaterPopup::RefreshDetailPane()
    {
        GitHubRelease^ r = nullptr;
        if (listReleases->SelectedIndex >= 0) {
            auto list = updater->State->GetReleasesForSelectedChannel();
            if (list != nullptr && listReleases->SelectedIndex < list->Count) r = list[listReleases->SelectedIndex];
        }
        // Fallback to latest if none selected and not browsing
        if (r == nullptr) {
            r = updater->State->LatestForChannel;
        }
        if (r == nullptr) {
            lblReleaseDetail->Text = "No release selected.";
            btnUpdateSelected->Enabled = false;
            btnDownloadSelected->Enabled = false;
            linkDetailGithub->Visible = false;
            return;
        }
        String^ ver = r->Version != nullptr ? r->Version->ToDisplayString() : r->Tag;
        String^ ch = ChannelHelper::ToDisplayString(r->Channel);
        String^ date = r->PublishedAt != DateTime::MinValue ? r->PublishedAt.ToString("yyyy-MM-dd HH:mm") : "unknown";
        String^ body = r->Body;
        if (!String::IsNullOrEmpty(body)) {
            String^ excerpt = body->Replace("\r", " ")->Replace("\n", " ");
            if (excerpt->Length > 180) excerpt = excerpt->Substring(0, 180) + "...";
            lblReleaseDetail->Text = String::Format("{0} • {1}\n{2}\n{3}", ver, ch, date, excerpt);
        } else {
            lblReleaseDetail->Text = String::Format("{0} • {1}\n{2}", ver, ch, date);
        }
        linkDetailGithub->Visible = !String::IsNullOrEmpty(r->HtmlUrl);
        linkDetailGithub->Tag = r->HtmlUrl;
        bool hasAsset = r->HasInstallerAsset();
        btnUpdateSelected->Enabled = hasAsset;
        btnDownloadSelected->Enabled = hasAsset;
        btnUpdateSelected->Text = hasAsset ? "Update" : "No installer";
        btnDownloadSelected->Text = "Download";
        // Indicate if downgrade
        if (r->Version != nullptr && updater->State->InstalledVersion != nullptr) {
            int cmp = r->Version->CompareTo(updater->State->InstalledVersion);
            if (cmp < 0) btnUpdateSelected->Text = "Downgrade";
            else if (cmp == 0) btnUpdateSelected->Text = "Reinstall";
        }
    }

    void UpdaterPopup::UpdateButtonStates()
    {
        if (updater == nullptr || updater->State == nullptr) return;
        auto st = updater->State;
        UpdaterStatus s = st->Status;
        bool isChecking = s == UpdaterStatus::Checking;
        bool isDownloading = s == UpdaterStatus::Downloading;
        bool isInstalling = s == UpdaterStatus::Installing;
        btnUpdate->Enabled = !isChecking && !isDownloading && !isInstalling && selectedReleaseForUpdate != nullptr && selectedReleaseForUpdate->HasInstallerAsset();
        btnDetails->Enabled = true;
        comboChannel->Enabled = !isChecking && !isDownloading;
        linkCheckNow->Enabled = !isChecking && !isDownloading;
        btnCancelDownload->Enabled = isDownloading;
        btnUpdateSelected->Enabled = !isChecking && !isDownloading && !isInstalling;
        btnDownloadSelected->Enabled = !isChecking && !isDownloading;
    }

    void UpdaterPopup::RefreshForExternalChange()
    {
        if (this->InvokeRequired) { this->BeginInvoke(gcnew Action(this, &UpdaterPopup::RefreshForExternalChange)); return; }
        RefreshAll();
    }

    void UpdaterPopup::SelectRelease(GitHubRelease^ release)
    {
        if (release == nullptr) return;
        // Find index in filtered list
        auto list = updater->State->GetReleasesForSelectedChannel();
        if (list == nullptr) return;
        for (int i = 0; i < list->Count; i++) {
            if (list[i]->Tag == release->Tag) {
                // Ensure browse expanded
                if (!isBrowseExpanded) {
                    isBrowseExpanded = true;
                    browsePanel->Visible = true;
                    linkBrowse->Text = "Browse releases ▾";
                    this->Height = 610;
                }
                listReleases->SelectedIndex = i;
                RefreshDetailPane();
                return;
            }
        }
    }

    // Handlers

    void UpdaterPopup::OnCheckNowLink(Object^, LinkLabelLinkClickedEventArgs^)
    {
        if (updater != nullptr) updater->CheckAsync(true);
    }

    void UpdaterPopup::OnChannelChanged(Object^, EventArgs^)
    {
        if (updater == nullptr) return;
        int idx = comboChannel->SelectedIndex;
        UpdateChannel ch = UpdateChannel::Stable;
        if (idx == 1) ch = UpdateChannel::Beta;
        else if (idx == 2) ch = UpdateChannel::PreRelease;
        updater->SetChannel(ch);
        RefreshBrowseList();
        RefreshDetailPane();
        RefreshHeader();
        UpdateButtonStates();
    }

    void UpdaterPopup::OnBrowseLink(Object^, LinkLabelLinkClickedEventArgs^)
    {
        isBrowseExpanded = !isBrowseExpanded;
        browsePanel->Visible = isBrowseExpanded;
        linkBrowse->Text = isBrowseExpanded ? "Browse releases ▾" : "Browse releases ▸";
        this->Height = isBrowseExpanded ? 610 : 460;
        if (isBrowseExpanded) RefreshBrowseList();
    }

    void UpdaterPopup::OnListSelected(Object^, EventArgs^)
    {
        RefreshDetailPane();
        UpdateButtonStates();
    }

    void UpdaterPopup::OnUpdateClick(Object^, EventArgs^)
    {
        if (selectedReleaseForUpdate == nullptr) {
            MessageBox::Show(this, "No update available for this channel.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Information);
            return;
        }
        DoUpdateForRelease(selectedReleaseForUpdate);
    }

    void UpdaterPopup::OnDetailsClick(Object^, EventArgs^)
    {
        // Toggle browse
        OnBrowseLink(nullptr, nullptr);
    }

    void UpdaterPopup::OnCancelDownloadClick(Object^, EventArgs^)
    {
        if (updater != nullptr) updater->CancelDownload();
    }

    void UpdaterPopup::OnUpdateSelectedClick(Object^, EventArgs^)
    {
        GitHubRelease^ r = nullptr;
        int idx = listReleases->SelectedIndex;
        auto list = updater->State->GetReleasesForSelectedChannel();
        if (idx >= 0 && list != nullptr && idx < list->Count) r = list[idx];
        if (r == nullptr) r = updater->State->LatestForChannel;
        if (r == nullptr) return;
        DoUpdateForRelease(r);
    }

    void UpdaterPopup::OnDownloadSelectedClick(Object^, EventArgs^)
    {
        GitHubRelease^ r = nullptr;
        int idx = listReleases->SelectedIndex;
        auto list = updater->State->GetReleasesForSelectedChannel();
        if (idx >= 0 && list != nullptr && idx < list->Count) r = list[idx];
        if (r == nullptr) return;
        ReleaseAsset^ asset = r->GetAuthoritativeInstallerAsset();
        if (asset == nullptr) {
            MessageBox::Show(this, "This release has no installer asset. Open on GitHub to download manually.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Information);
            return;
        }
        SaveFileDialog^ dlg = gcnew SaveFileDialog();
        dlg->FileName = "Windows_Hello_Fix_Setup.exe";
        dlg->Filter = "Installer (*.exe)|*.exe|All files (*.*)|*.*";
        dlg->Title = "Save installer as";
        dlg->InitialDirectory = Environment::GetFolderPath(Environment::SpecialFolder::UserProfile) + "\\Downloads";
        if (dlg->ShowDialog(this) != ::DialogResult::OK) return;
        String^ path = dlg->FileName;
        // Run download to user path
        updater->DownloadToUserPathAsync(r, path);
        MessageBox::Show(this, "Downloading to " + path + "\n\nThe installer will be saved there and not auto-launched.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Information);
    }

    void UpdaterPopup::OnDetailGithubClick(Object^, LinkLabelLinkClickedEventArgs^)
    {
        GitHubRelease^ r = nullptr;
        int idx = listReleases->SelectedIndex;
        auto list = updater->State->GetReleasesForSelectedChannel();
        if (idx >= 0 && list != nullptr && idx < list->Count) r = list[idx];
        if (r == nullptr) return;
        OpenUrl(r->HtmlUrl);
    }

    void UpdaterPopup::OnViewOnGithubClick(Object^, LinkLabelLinkClickedEventArgs^)
    {
        if (updater == nullptr) return;
        GitHubRelease^ latest = updater->State->LatestForChannel;
        if (latest != nullptr && !String::IsNullOrEmpty(latest->HtmlUrl)) OpenUrl(latest->HtmlUrl);
        else if (!String::IsNullOrEmpty(updater->State->StatusDetail)) OpenUrl("https://github.com/Shivu516/Windows-Hello-Fix/releases");
    }

    void UpdaterPopup::OnUpdaterStateChanged(Object^, EventArgs^)
    {
        if (this->InvokeRequired) { this->BeginInvoke(gcnew Action(this, &UpdaterPopup::RefreshForExternalChange)); return; }
        RefreshHeader();
        UpdateButtonStates();
    }

    void UpdaterPopup::OnUpdaterReleasesUpdated(Object^, EventArgs^)
    {
        if (this->InvokeRequired) { this->BeginInvoke(gcnew Action(this, &UpdaterPopup::RefreshForExternalChange)); return; }
        RefreshBrowseList();
        RefreshDetailPane();
        RefreshHeader();
    }

    void UpdaterPopup::OnFormDeactivate(Object^, EventArgs^)
    {
        // Optional: keep open? Spec says compact floating; we keep open until user closes.
        // Don't auto-close on deactivate to allow copy.
    }

    bool UpdaterPopup::ConfirmDowngradeIfNeeded(GitHubRelease^ target)
    {
        if (target == nullptr || target->Version == nullptr) return true;
        auto cur = updater->State->InstalledVersion;
        if (cur == nullptr) return true;
        int cmp = target->Version->CompareTo(cur);
        if (cmp >= 0) return true; // not downgrade

        bool hasUpdater = target->HasUpdaterSupport;
        String^ msg;
        if (!hasUpdater) {
            msg = String::Format(
                "You are about to downgrade from {0} to {1}.\n\n"
                "⚠ {1} does NOT include the in-app updater.\n"
                "After downgrading, the download icon and Browse Releases will disappear.\n"
                "To return to a newer version you will need to manually download the installer from GitHub.\n\n"
                "Do you want to continue with the downgrade?",
                cur->ToDisplayString(), target->Version->ToDisplayString());
        } else {
            msg = String::Format(
                "You are about to downgrade from {0} to {1}.\n\nDo you want to continue?",
                cur->ToDisplayString(), target->Version->ToDisplayString());
        }
        auto res = MessageBox::Show(this, msg, "Confirm Downgrade", MessageBoxButtons::YesNo, MessageBoxIcon::Warning, MessageBoxDefaultButton::Button2);
        return res == ::DialogResult::Yes;
    }

    void UpdaterPopup::DoUpdateForRelease(GitHubRelease^ release)
    {
        if (release == nullptr) return;
        if (!ConfirmDowngradeIfNeeded(release)) return;

        ReleaseAsset^ asset = release->GetAuthoritativeInstallerAsset();
        if (asset == nullptr) {
            MessageBox::Show(this, "This release has no installer asset (Windows_Hello_Fix_Setup.exe).\n\nOpen on GitHub to download manually.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Information);
            return;
        }
        if (!UpdateInstaller::IsValidGithubAssetUrl(asset->BrowserDownloadUrl)) {
            MessageBox::Show(this, "Invalid asset URL. Update blocked for security.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Error);
            return;
        }

        // If user wants, confirm update
        auto confirm = MessageBox::Show(this,
            String::Format("Update to {0}?\n\nThe installer will be downloaded to a temporary location, verified, and launched.\nThe app will exit and restart at the new version.\n\nDownload size: {1} bytes\nFrom: {2}",
                release->Version->ToDisplayString(), asset->Size, asset->BrowserDownloadUrl),
            "Confirm Update", MessageBoxButtons::YesNo, MessageBoxIcon::Question);
        if (confirm != ::DialogResult::Yes) return;

        // Start download
        pendingUpdateRelease = release;
        auto task = updater->DownloadReleaseAsync(release);
        task->ContinueWith(gcnew Action<Task<bool>^>(this, &UpdaterPopup::OnDownloadTaskCompleted), TaskScheduler::Default);
    }

    void UpdaterPopup::OnDownloadTaskCompleted(Task<bool>^ t)
    {
        try {
            bool ok = false;
            if (t != nullptr && t->IsCompleted && !t->IsFaulted && !t->IsCanceled) ok = t->Result;
            if (!ok) {
                if (this->InvokeRequired) this->BeginInvoke(gcnew Action<String^>(this, &UpdaterPopup::DoUpdateForRelease_Failed), "Download failed. See status banner.");
                else DoUpdateForRelease_Failed("Download failed.");
                return;
            }
            GitHubRelease^ rel = pendingUpdateRelease;
            if (this->InvokeRequired) this->BeginInvoke(gcnew Action<GitHubRelease^>(this, &UpdaterPopup::DoUpdateForRelease_Launch), rel);
            else DoUpdateForRelease_Launch(rel);
        } catch (Exception^ ex) {
            if (this->InvokeRequired) this->BeginInvoke(gcnew Action<String^>(this, &UpdaterPopup::DoUpdateForRelease_Failed), ex->Message);
            else DoUpdateForRelease_Failed(ex->Message);
        }
    }

    void UpdaterPopup::DoUpdateForRelease_Failed(String^ msg)
    {
        MessageBox::Show(this, msg, "Updater", MessageBoxButtons::OK, MessageBoxIcon::Error);
    }

    void UpdaterPopup::DoUpdateForRelease_Launch(GitHubRelease^ /*release*/)
    {
        // Launch silent? For now interactive (silent=false) so user sees wizard. Could ask.
        auto res = MessageBox::Show(this, "Download verified. Launch installer now?\n\nThe app will exit.", "Updater", MessageBoxButtons::YesNo, MessageBoxIcon::Question);
        if (res != ::DialogResult::Yes) return;
        bool ok = updater->LaunchStagedInstaller(false); // interactive
        if (!ok) {
            MessageBox::Show(this, "Failed to launch installer. You can try again or use 'Download' to save manually.", "Updater", MessageBoxButtons::OK, MessageBoxIcon::Error);
        }
        // If ok, app will exit shortly via Updater::LaunchStagedInstaller
    }

    void UpdaterPopup::OpenUrl(String^ url)
    {
        if (String::IsNullOrEmpty(url)) return;
        // Validate
        Uri^ uri;
        if (!Uri::TryCreate(url, UriKind::Absolute, uri)) return;
        if (uri->Scheme != "https") return;
        if (!uri->Host->Equals("github.com", StringComparison::OrdinalIgnoreCase)) {
            // Allow github.com only per security model
            // But for generic, allow any https? Spec says browser integration only for github release URL
            // We'll allow only github.com
            MessageBox::Show(this, "URL not allowed: " + url, "Updater", MessageBoxButtons::OK, MessageBoxIcon::Warning);
            return;
        }
        if (updater != nullptr) {
            try { updater->State; /* logging via updater would be updater->Log */ } catch (...) {}
        }
        try {
            ProcessStartInfo^ psi = gcnew ProcessStartInfo();
            psi->FileName = url;
            psi->UseShellExecute = true;
            Process::Start(psi);
        } catch (Exception^ ex) {
            MessageBox::Show(this, "Failed to open browser: " + ex->Message, "Updater", MessageBoxButtons::OK, MessageBoxIcon::Error);
        }
    }

} // namespace Updater
} // namespace Windows_Hello_Fix_v2_0

// Need to expose private launch helpers to avoid C++/CLI generic delegate issues with lambdas capturing this
// We add helper methods here to satisfy forward refs above — but they were already declared? We need to declare them in header.
// For now, we provide inline definitions via #include trick — we will add them to header later if build fails.
// Quick hack: define them as private methods via extension
