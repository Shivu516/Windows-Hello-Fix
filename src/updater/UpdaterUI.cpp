#undef Rectangle
#undef GetTempPath
#include <windows.h>
#include "UpdaterUI.h"
#include "UpdateInstaller.h"
#include "HtmlRenderer.h"
using namespace System;
using namespace System::Drawing;
using namespace System::Windows::Forms;
using namespace System::Diagnostics;
using namespace System::Threading::Tasks;
using namespace System::Threading::Tasks;
namespace Windows_Hello_Fix_v2_0 { namespace Updater {
UpdaterUI::UpdaterUI(Form^ owner, Updater^ updater) : ownerForm(owner), updater(updater), pulseOn(false), iconInstalled(false), popup(nullptr) {
 iconPanel = gcnew Panel();
 iconPanel->Size = Drawing::Size(20,20);
 iconPanel->BackColor = Color::Transparent;
 iconPanel->Cursor = Cursors::Hand;
 tooltip = gcnew ToolTip();
 tooltip->AutoPopDelay = 5000; tooltip->InitialDelay = 200; tooltip->ReshowDelay = 100; tooltip->ShowAlways = false;
 pulseTimer = gcnew Timer(); pulseTimer->Interval = 500; pulseTimer->Tick += gcnew EventHandler(this, &UpdaterUI::OnPulseTick);
 iconPanel->Paint += gcnew PaintEventHandler(this, &UpdaterUI::OnIconPaint);
 iconPanel->Click += gcnew EventHandler(this, &UpdaterUI::OnIconClick);
 iconPanel->MouseEnter += gcnew EventHandler(this, &UpdaterUI::OnIconMouseEnter);
 iconPanel->MouseLeave += gcnew EventHandler(this, &UpdaterUI::OnIconMouseLeave);
 if (updater != nullptr) updater->StateChanged += gcnew EventHandler(this, &UpdaterUI::OnStateChanged);
}
UpdaterUI::~UpdaterUI() { this->!UpdaterUI(); }
UpdaterUI::!UpdaterUI() { try { if (pulseTimer != nullptr) pulseTimer->Stop(); } catch(...) {} try { RemoveIcon(); } catch(...) {} try { if (popup != nullptr && !popup->IsDisposed) { popup->Close(); popup=nullptr; } } catch(...) {} }
float UpdaterUI::GetScaleFactor(){ try { if (ownerForm!=nullptr && !ownerForm->IsDisposed && ownerForm->Handle!=IntPtr::Zero) { Graphics^ g = ownerForm->CreateGraphics(); float dpi = g->DpiX; delete g; return dpi/96.0f; } } catch(...) {} return 1.0f; }
Point UpdaterUI::ComputeIconLocation(){ if(ownerForm==nullptr) return Point(0,0); float scale=GetScaleFactor(); int baseSize=(int)Math::Round(20*scale); int x = ownerForm->ClientSize.Width - (int)Math::Round(28*scale) - baseSize + 20; int y = (int)Math::Round(192*scale); if(x<0) x=0; if(y<0) y=0; return Point(x,y); }
void UpdaterUI::EnsureIconPosition(){ if(iconPanel==nullptr||ownerForm==nullptr) return; float scale=GetScaleFactor(); int sz=(int)Math::Round(20*scale); iconPanel->Size = Drawing::Size(sz,sz); Point p=ComputeIconLocation(); iconPanel->Location=p; iconPanel->BringToFront(); }
void UpdaterUI::InstallIcon(){ if(iconInstalled) return; if(ownerForm==nullptr||ownerForm->IsDisposed) return; try{ if(ownerForm->InvokeRequired){ ownerForm->BeginInvoke(gcnew MethodInvoker(this,&UpdaterUI::InstallIcon)); return; } ownerForm->Controls->Add(iconPanel); EnsureIconPosition(); ownerForm->Resize += gcnew EventHandler(this,&UpdaterUI::OnOwnerResize); iconInstalled=true; RefreshIcon(); pulseTimer->Start(); }catch(...) {}}
void UpdaterUI::RemoveIcon(){ if(!iconInstalled) return; try{ if(ownerForm!=nullptr&&!ownerForm->IsDisposed){ if(ownerForm->InvokeRequired){ try{ ownerForm->BeginInvoke(gcnew MethodInvoker(this,&UpdaterUI::RemoveIcon)); }catch(...) {} return; } if(ownerForm->Controls->Contains(iconPanel)) ownerForm->Controls->Remove(iconPanel); } iconInstalled=false; pulseTimer->Stop(); }catch(...) {}}
void UpdaterUI::RefreshIcon(){ try{ if(iconPanel!=nullptr&&!iconPanel->IsDisposed){ iconPanel->Invalidate(); String^ tip=GetTooltipForStatus(); tooltip->SetToolTip(iconPanel,tip); } }catch(...) {}}
String^ UpdaterUI::GetTooltipForStatus(){ if(updater==nullptr||updater->State==nullptr) return "Updates — click to view releases"; auto st=updater->State; UpdaterStatus s=st->Status; String^ detail=st->StatusDetail; GitHubRelease^ latest=st->LatestForChannel; String^ ver=latest!=nullptr&&latest->Version!=nullptr?latest->Version->ToDisplayString():""; switch(s){ case UpdaterStatus::Checking: return "Checking for updates..."; case UpdaterStatus::UpdateAvailable: return String::Format("Update available: {0} — click to update",ver); case UpdaterStatus::Downloading: return String::Format("Downloading {0}% — click to view progress",st->DownloadProgress); case UpdaterStatus::Installing: return "Installing — app will restart"; case UpdaterStatus::UpToDate: return String::Format("Up to date — {0}",UpdateVersion::CurrentDisplayString()); case UpdaterStatus::Offline: return "Offline — update info unavailable. Click to retry."; case UpdaterStatus::RateLimited: return "Rate limited — click to retry. "+detail; case UpdaterStatus::Error: return "Update check failed — click to retry. "+detail; default: return "Updates — click to view releases"; } }
void UpdaterUI::OnPulseTick(Object^,EventArgs^){ if(updater==nullptr) return; auto s=updater->State->Status; if(s==UpdaterStatus::Checking||s==UpdaterStatus::Downloading){ pulseOn=!pulseOn; if(iconPanel!=nullptr&&!iconPanel->IsDisposed) iconPanel->Invalidate(); } }
void UpdaterUI::OnStateChanged(Object^,EventArgs^){ try{ if(ownerForm!=nullptr&&!ownerForm->IsDisposed&&ownerForm->InvokeRequired) ownerForm->BeginInvoke(gcnew MethodInvoker(this,&UpdaterUI::RefreshIcon)); else RefreshIcon(); }catch(...) {} try{ if(popup!=nullptr&&!popup->IsDisposed){ if(popup->InvokeRequired) popup->BeginInvoke(gcnew MethodInvoker(popup,&UpdaterPopup::RefreshForExternalChange)); else popup->RefreshForExternalChange(); } }catch(...) {} }
void UpdaterUI::OnIconPaint(Object^ sender, PaintEventArgs^ e){ try{ Panel^ p=safe_cast<Panel^>(sender); Graphics^ g=e->Graphics; g->SmoothingMode=Drawing2D::SmoothingMode::AntiAlias; g->TextRenderingHint=Drawing::Text::TextRenderingHint::SystemDefault; auto st=updater!=nullptr?updater->State:nullptr; UpdaterStatus s=st!=nullptr?st->Status:UpdaterStatus::Idle; bool hasUpdate=st!=nullptr&&s==UpdaterStatus::UpdateAvailable; bool isChecking=s==UpdaterStatus::Checking; bool isDownloading=s==UpdaterStatus::Downloading; bool isError=s==UpdaterStatus::Error||s==UpdaterStatus::Offline||s==UpdaterStatus::RateLimited; System::Drawing::Rectangle rect=p->ClientRectangle; float scale=GetScaleFactor(); Color glyphColor = SystemColors::ControlText; if(hasUpdate) glyphColor=Color::FromArgb(0,90,158); else if(isError) glyphColor=Color::FromArgb(130,130,130); else if(isChecking&&pulseOn) glyphColor=Color::FromArgb(180,180,180); else if(isChecking) glyphColor=Color::FromArgb(120,120,120); float pad=2*scale; float w=rect.Width - 2*pad; float h=rect.Height - 2*pad; // Vector download arrow: tray + stem + head
Pen^ pen = gcnew Pen(glyphColor, 1.5f*scale); pen->LineJoin=Drawing2D::LineJoin::Round; pen->EndCap=Drawing2D::LineCap::Round; // Tray
g->DrawRectangle(pen, pad+2*scale, pad+13*scale, w-4*scale, 2.5f*scale); // Stem
g->DrawLine(pen, rect.Width/2.0f, pad+3*scale, rect.Width/2.0f, pad+12*scale); // Head
array<PointF>^ pts = gcnew array<PointF>(3); pts[0]=PointF(rect.Width/2.0f - 5*scale, pad+9*scale); pts[1]=PointF(rect.Width/2.0f + 5*scale, pad+9*scale); pts[2]=PointF(rect.Width/2.0f, pad+15*scale); SolidBrush^ br = gcnew SolidBrush(glyphColor); g->FillPolygon(br, pts); delete pen; delete br; if(isDownloading && st!=nullptr){ int pct=st->DownloadProgress; Pen^ bgPen=gcnew Pen(Color::FromArgb(220,220,220), 1.5f*scale); g->DrawEllipse(bgPen, System::Drawing::Rectangle((int)pad,(int)pad,(int)w,(int)h)); Pen^ fgPen=gcnew Pen(Color::FromArgb(0,120,212), 1.5f*scale); float sweep=(pct/100.0f)*360.0f; g->DrawArc(fgPen, System::Drawing::Rectangle((int)pad,(int)pad,(int)w,(int)h), -90, sweep); delete bgPen; delete fgPen; } else if(s==UpdaterStatus::Installing){ Pen^ pen2=gcnew Pen(Color::FromArgb(0,120,212),1.5f*scale); float angle=(Environment::TickCount%1000)/1000.0f*360.0f; g->DrawArc(pen2, System::Drawing::Rectangle((int)pad,(int)pad,(int)w,(int)h), angle, 270); delete pen2; } if(isError){ Font^ sf=gcnew Font("Segoe UI", 7*scale, FontStyle::Bold); SolidBrush^ rbr=gcnew SolidBrush(Color::FromArgb(209,52,56)); g->DrawString("!", sf, rbr, PointF(rect.Width - 10*scale, 0)); delete sf; delete rbr; } if(hasUpdate){ float dotDp=6.0f*scale; float dotX=rect.Width - dotDp - 1*scale; float dotY=1*scale; SolidBrush^ dotBr=gcnew SolidBrush(Color::FromArgb(209,52,56)); Pen^ dotPen=gcnew Pen(Color::White, 1*scale); g->FillEllipse(dotBr, dotX, dotY, dotDp, dotDp); g->DrawEllipse(dotPen, dotX, dotY, dotDp, dotDp); delete dotBr; delete dotPen; } }catch(...) {}}
void UpdaterUI::OnIconClick(Object^,EventArgs^){ try{ ShowPopup(); }catch(...) {}}
void UpdaterUI::OnIconMouseEnter(Object^,EventArgs^){ try{ if(iconPanel!=nullptr) iconPanel->Invalidate(); }catch(...) {}}
void UpdaterUI::OnIconMouseLeave(Object^,EventArgs^){ try{ if(iconPanel!=nullptr) iconPanel->Invalidate(); }catch(...) {}}
void UpdaterUI::OnOwnerResize(Object^,EventArgs^){ try{ EnsureIconPosition(); }catch(...) {}}
void UpdaterUI::OnPopupClosed(Object^,FormClosedEventArgs^){ try{ popup=nullptr; }catch(...) {}}
void UpdaterUI::ShowPopup(){ try{ if(ownerForm==nullptr||ownerForm->IsDisposed) return; if(ownerForm->InvokeRequired){ ownerForm->BeginInvoke(gcnew MethodInvoker(this,&UpdaterUI::ShowPopup)); return; } if(popup==nullptr||popup->IsDisposed){ popup=gcnew UpdaterPopup(ownerForm, updater); popup->FormClosed+=gcnew FormClosedEventHandler(this,&UpdaterUI::OnPopupClosed); } if(!popup->Visible){ System::Drawing::Point ownerLoc=ownerForm->Location; System::Drawing::Size ownerSz=ownerForm->Size; int x=ownerLoc.X+ownerSz.Width-popup->Width-10; int y=ownerLoc.Y+ownerSz.Height-10; System::Drawing::Rectangle work=Screen::FromControl(ownerForm)->WorkingArea; if(x+popup->Width>work.Right) x=work.Right-popup->Width-10; if(y+popup->Height>work.Bottom) y=work.Bottom-popup->Height-10; if(x<work.Left) x=work.Left+10; if(y<work.Top) y=work.Top+10; popup->StartPosition=FormStartPosition::Manual; popup->Location=System::Drawing::Point(x,y); popup->Show(ownerForm); } popup->BringToFront(); popup->Activate(); popup->RefreshForExternalChange(); }catch(Exception^ ex){ try{ MessageBox::Show(ownerForm,"Updater popup failed: "+ex->Message,"Updater",MessageBoxButtons::OK,MessageBoxIcon::Error); }catch(...) {} }}
void UpdaterUI::ShowPopupForRelease(GitHubRelease^ release){ ShowPopup(); try{ if(popup!=nullptr&&!popup->IsDisposed) popup->SelectRelease(release); }catch(...) {}}
bool UpdaterUI::IsPopupOpen(){ try{ return popup!=nullptr&&!popup->IsDisposed&&popup->Visible; }catch(...){ return false; } }
UpdaterPopup::UpdaterPopup(Form^ owner, Updater^ updater) : ownerForm(owner), updater(updater), selectedRelease(nullptr), pendingUpdateRelease(nullptr) {
 InitializeComponentPopup();
 if(updater!=nullptr){ updater->StateChanged+=gcnew EventHandler(this,&UpdaterPopup::OnUpdaterStateChanged); updater->ReleasesUpdated+=gcnew EventHandler(this,&UpdaterPopup::OnUpdaterReleasesUpdated); }
 this->Deactivate+=gcnew EventHandler(this,&UpdaterPopup::OnFormDeactivate);
 RefreshAll();
}
void UpdaterPopup::InitializeComponentPopup(){
 this->SuspendLayout();
 this->FormBorderStyle = ::System::Windows::Forms::FormBorderStyle::FixedDialog;
 this->MaximizeBox=false; this->MinimizeBox=false; this->ShowInTaskbar=false; this->ShowIcon=false; this->TopMost=false;
 this->StartPosition=::System::Windows::Forms::FormStartPosition::Manual;
 this->Size=System::Drawing::Size(360,420); this->MinimumSize=System::Drawing::Size(360,420); this->MaximumSize=System::Drawing::Size(360,520);
 this->BackColor=SystemColors::Window; this->ForeColor=SystemColors::WindowText;
 this->Font=gcnew Drawing::Font("Segoe UI", 9);
 this->DoubleBuffered=true;
 this->Text="Updates";
 int pad=12; int y=pad;
 Label^ lblCap = gcnew Label(); lblCap->Location=Point(pad,y); lblCap->Size=System::Drawing::Size(60,21); lblCap->Text="Release"; lblCap->ForeColor=Color::FromArgb(96,94,92); lblCap->Font=gcnew Drawing::Font("Segoe UI",8); lblCap->TextAlign=ContentAlignment::MiddleLeft;
 cmbRelease = gcnew ComboBox(); cmbRelease->DropDownStyle=ComboBoxStyle::DropDownList; cmbRelease->Location=Point(pad+62,y); cmbRelease->Size=System::Drawing::Size(200,21); cmbRelease->Font=gcnew Drawing::Font("Segoe UI",9); cmbRelease->SelectedIndexChanged+=gcnew EventHandler(this,&UpdaterPopup::OnReleaseChanged);
 y+=28;
 lblStatus = gcnew Label(); lblStatus->Location=Point(pad,y); lblStatus->Size=System::Drawing::Size(336,16); lblStatus->Font=gcnew Drawing::Font("Segoe UI",9); lblStatus->AutoSize=false;
 y+=18;
 pnlWarning = gcnew Panel(); pnlWarning->Location=Point(pad,y); pnlWarning->Size=System::Drawing::Size(336,36); pnlWarning->BackColor=Color::FromArgb(255,248,225); pnlWarning->BorderStyle=BorderStyle::FixedSingle; pnlWarning->Visible=false;
 lblWarning = gcnew Label(); lblWarning->Location=Point(4,4); lblWarning->Size=System::Drawing::Size(328,28); lblWarning->Font=gcnew Drawing::Font("Segoe UI",8); lblWarning->ForeColor=Color::FromArgb(102,60,0); lblWarning->Text="⚠ This version does not include the in-app updater. Future updates will require manual download from GitHub."; lblWarning->AutoSize=false;
 pnlWarning->Controls->Add(lblWarning); y+=40;
 btnUpdate = gcnew Button(); btnUpdate->Location=Point(pad,y); btnUpdate->Size=System::Drawing::Size(110,28); btnUpdate->Text="Update"; btnUpdate->UseVisualStyleBackColor=true; btnUpdate->Click+=gcnew EventHandler(this,&UpdaterPopup::OnUpdateClick);
 y+=34;
 lblNotesHeader = gcnew Label(); lblNotesHeader->Location=Point(pad,y); lblNotesHeader->Size=System::Drawing::Size(336,16); lblNotesHeader->Text="Release notes"; lblNotesHeader->Font=gcnew Drawing::Font("Segoe UI",8,FontStyle::Bold); lblNotesHeader->ForeColor=Color::FromArgb(96,94,92);
 y+=16;
 separator = gcnew Panel(); separator->Location=Point(pad,y); separator->Size=System::Drawing::Size(336,1); separator->BackColor=Color::FromArgb(225,225,225); y+=6;
 rtbNotes = gcnew RichTextBox(); rtbNotes->Location=Point(pad,y); rtbNotes->Size=System::Drawing::Size(336,140); rtbNotes->ReadOnly=true; rtbNotes->ScrollBars=RichTextBoxScrollBars::Vertical; rtbNotes->BorderStyle=BorderStyle::FixedSingle; rtbNotes->BackColor=SystemColors::Window; rtbNotes->ForeColor=SystemColors::WindowText; rtbNotes->WordWrap=true; rtbNotes->DetectUrls=true; rtbNotes->Font=gcnew Drawing::Font("Segoe UI",9); rtbNotes->LinkClicked+=gcnew LinkClickedEventHandler(this,&UpdaterPopup::OnNotesLinkClicked);
 y+=146;
 linkViewOnGithub = gcnew LinkLabel(); linkViewOnGithub->Location=Point(pad,y); linkViewOnGithub->AutoSize=true; linkViewOnGithub->Text="View on GitHub"; linkViewOnGithub->LinkClicked+=gcnew LinkLabelLinkClickedEventHandler(this,&UpdaterPopup::OnViewOnGithubClick);
 y+=18;
 progressBar = gcnew ProgressBar(); progressBar->Location=Point(pad,y); progressBar->Size=System::Drawing::Size(336,8); progressBar->Visible=false; progressBar->Style=ProgressBarStyle::Continuous; y+=12;
 lblStatusBanner = gcnew Label(); lblStatusBanner->Location=Point(pad,y); lblStatusBanner->Size=System::Drawing::Size(336,16); lblStatusBanner->AutoSize=false; lblStatusBanner->Visible=false;
 this->Controls->Add(lblCap); this->Controls->Add(cmbRelease); this->Controls->Add(lblStatus); this->Controls->Add(pnlWarning); this->Controls->Add(btnUpdate); this->Controls->Add(lblNotesHeader); this->Controls->Add(separator); this->Controls->Add(rtbNotes); this->Controls->Add(linkViewOnGithub); this->Controls->Add(progressBar); this->Controls->Add(lblStatusBanner);
 this->ResumeLayout(false); this->PerformLayout();
}
void UpdaterPopup::RefreshAll(){ RefreshStatus(); RefreshReleaseList(); RefreshNotes(); UpdateButtonStates(); }
void UpdaterPopup::RefreshStatus(){
 if(updater==nullptr||updater->State==nullptr) return; auto st=updater->State; String^ cur=UpdateVersion::CurrentDisplayString(); GitHubRelease^ latest=st->LatestForChannel; bool isLatestAvail=st->IsUpdateAvailable(); UpdaterStatus s=st->Status;
 if(isLatestAvail && latest!=nullptr && latest->Version!=nullptr){ lblStatus->Text=String::Format("⚠ Newer version available: {0}", latest->Version->ToDisplayString()); lblStatus->ForeColor=Color::FromArgb(152,111,11); }
 else { lblStatus->Text=String::Format("✓ You are on the latest version: {0}", cur); lblStatus->ForeColor=Color::FromArgb(16,124,16); }
 // Status banner for checking/downloading/error/offline
 lblStatusBanner->Visible=true;
 switch(s){
  case UpdaterStatus::Checking: lblStatusBanner->Text="Checking for updates..."; lblStatusBanner->ForeColor=Color::FromArgb(0,90,158); progressBar->Visible=false; break;
  case UpdaterStatus::Downloading: lblStatusBanner->Text=String::Format("Downloading {0}%...", st->DownloadProgress); lblStatusBanner->ForeColor=Color::FromArgb(0,90,158); progressBar->Visible=true; progressBar->Value=Math::Min(100,Math::Max(0,st->DownloadProgress)); break;
  case UpdaterStatus::Installing: lblStatusBanner->Text="Installing — app will restart..."; lblStatusBanner->ForeColor=Color::FromArgb(16,124,16); progressBar->Visible=false; break;
  case UpdaterStatus::Offline: lblStatusBanner->Text="Offline — using cached info."; lblStatusBanner->ForeColor=Color::FromArgb(209,52,56); progressBar->Visible=false; break;
  case UpdaterStatus::RateLimited: lblStatusBanner->Text="Rate limited — try again later. "+st->StatusDetail; lblStatusBanner->ForeColor=Color::FromArgb(209,52,56); progressBar->Visible=false; break;
  case UpdaterStatus::Error: lblStatusBanner->Text="Update check failed: "+st->StatusDetail; lblStatusBanner->ForeColor=Color::FromArgb(209,52,56); progressBar->Visible=false; break;
  default: lblStatusBanner->Visible=false; progressBar->Visible=false; break;
 }
}
void UpdaterPopup::RefreshReleaseList(){
 if(updater==nullptr||updater->State==nullptr) return; auto st=updater->State; auto list=st->GetAllReleasesSorted(); String^ prevTag = selectedRelease!=nullptr?selectedRelease->Tag:nullptr; cmbRelease->BeginUpdate(); cmbRelease->Items->Clear();
 if(list!=nullptr){ for each(GitHubRelease^ r in list){ String^ tag=r->Tag!=nullptr?r->Tag:"unknown"; String^ mark=""; if(r->Version!=nullptr&&st->InstalledVersion!=nullptr&&r->Version->CompareTo(st->InstalledVersion)==0) mark=" ✓ installed"; String^ display=tag+mark; cmbRelease->Items->Add(display); } } cmbRelease->EndUpdate();
 if(cmbRelease->Items->Count>0){
  int selIdx=0;
  if(prevTag!=nullptr){ for(int i=0;i<list->Count;i++){ if(list[i]->Tag==prevTag){ selIdx=i; break; } } }
  else if(list!=nullptr && list->Count>0){
   // Default to latest available if update available, else installed version or newest
   GitHubRelease^ latest=st->LatestForChannel; if(latest!=nullptr){ for(int i=0;i<list->Count;i++){ if(list[i]->Tag==latest->Tag){ selIdx=i; break; } } }
  }
  cmbRelease->SelectedIndex=selIdx;
 }
}
void UpdaterPopup::RefreshNotes(){
 GitHubRelease^ r=nullptr; if(cmbRelease->SelectedIndex>=0){ auto list=updater->State->GetAllReleasesSorted(); if(list!=nullptr&&cmbRelease->SelectedIndex < list->Count) r=list[cmbRelease->SelectedIndex]; }
 if(r==nullptr) r=updater->State->LatestForChannel;
 selectedRelease=r;
 if(r==nullptr){ rtbNotes->Clear(); rtbNotes->AppendText("No release selected."); pnlWarning->Visible=false; linkViewOnGithub->Visible=false; return; }
 pnlWarning->Visible=!r->HasUpdaterSupport;
 linkViewOnGithub->Visible=!String::IsNullOrEmpty(r->HtmlUrl); linkViewOnGithub->Tag=r->HtmlUrl;
 // Render via HTML pipeline: markdown → controlled HTML → RichTextBox
 try{ HtmlRenderer::Render(rtbNotes, r->Body); }catch(...){ try{ rtbNotes->Text=r->Body!=nullptr?r->Body:""; }catch(...) {}}
}
void UpdaterPopup::UpdateButtonStates(){
 if(updater==nullptr||updater->State==nullptr) return; auto st=updater->State; UpdaterStatus s=st->Status; bool isChecking=s==UpdaterStatus::Checking; bool isDownloading=s==UpdaterStatus::Downloading; bool isInstalling=s==UpdaterStatus::Installing;
 if(selectedRelease==nullptr){ btnUpdate->Enabled=false; btnUpdate->Text="Update"; return; }
 bool hasAsset=selectedRelease->HasInstallerAsset(); btnUpdate->Enabled=!isChecking&&!isDownloading&&!isInstalling&&hasAsset;
 if(!hasAsset) btnUpdate->Text="No installer";
 else if(selectedRelease->Version!=nullptr&&st->InstalledVersion!=nullptr){
  int cmp=selectedRelease->Version->CompareTo(st->InstalledVersion); if(cmp<0) btnUpdate->Text="Downgrade"; else if(cmp==0) btnUpdate->Text="Reinstall"; else btnUpdate->Text="Update";
 } else btnUpdate->Text="Update";
}
void UpdaterPopup::RefreshForExternalChange(){ if(this->InvokeRequired){ this->BeginInvoke(gcnew MethodInvoker(this,&UpdaterPopup::RefreshForExternalChange)); return; } RefreshAll(); }
void UpdaterPopup::SelectRelease(GitHubRelease^ release){ if(release==nullptr) return; auto list=updater->State->GetAllReleasesSorted(); if(list==nullptr) return; for(int i=0;i<list->Count;i++){ if(list[i]->Tag==release->Tag){ cmbRelease->SelectedIndex=i; RefreshNotes(); return; } } }
void UpdaterPopup::OnReleaseChanged(Object^,EventArgs^){ RefreshNotes(); UpdateButtonStates(); RefreshStatus(); }
void UpdaterPopup::OnUpdateClick(Object^,EventArgs^){ if(selectedRelease==nullptr){ MessageBox::Show(this,"No release selected.","Updater",MessageBoxButtons::OK,MessageBoxIcon::Information); return; } DoUpdateForRelease(selectedRelease); }
void UpdaterPopup::OnViewOnGithubClick(Object^,LinkLabelLinkClickedEventArgs^){ if(selectedRelease!=nullptr&&!String::IsNullOrEmpty(selectedRelease->HtmlUrl)) OpenUrl(selectedRelease->HtmlUrl); else if(!String::IsNullOrEmpty(updater->State->StatusDetail)) OpenUrl("https://github.com/Shivu516/Windows-Hello-Fix/releases"); }
void UpdaterPopup::OnNotesLinkClicked(Object^,LinkClickedEventArgs^ e){ OpenUrl(e->LinkText); }
void UpdaterPopup::OnUpdaterStateChanged(Object^,EventArgs^){ if(this->InvokeRequired){ this->BeginInvoke(gcnew MethodInvoker(this,&UpdaterPopup::RefreshForExternalChange)); return; } RefreshStatus(); UpdateButtonStates(); }
void UpdaterPopup::OnUpdaterReleasesUpdated(Object^,EventArgs^){ if(this->InvokeRequired){ this->BeginInvoke(gcnew MethodInvoker(this,&UpdaterPopup::RefreshForExternalChange)); return; } RefreshReleaseList(); RefreshNotes(); RefreshStatus(); }
void UpdaterPopup::OnFormDeactivate(Object^,EventArgs^){}
bool UpdaterPopup::ConfirmDowngradeIfNeeded(GitHubRelease^ target){ if(target==nullptr||target->Version==nullptr) return true; auto cur=updater->State->InstalledVersion; if(cur==nullptr) return true; int cmp=target->Version->CompareTo(cur); if(cmp>=0) return true; bool hasUpdater=target->HasUpdaterSupport; String^ msg; if(!hasUpdater){ msg=String::Format("You are about to downgrade from {0} to {1}.\n\n⚠ {1} does NOT include the in-app updater.\nAfter downgrading, the download icon and Browse Releases will disappear.\nTo return to a newer version you will need to manually download the installer from GitHub.\n\nDo you want to continue with the downgrade?", cur->ToDisplayString(), target->Version->ToDisplayString()); } else { msg=String::Format("You are about to downgrade from {0} to {1}.\n\nDo you want to continue?", cur->ToDisplayString(), target->Version->ToDisplayString()); } auto res=MessageBox::Show(this, msg, "Confirm Downgrade", MessageBoxButtons::YesNo, MessageBoxIcon::Warning, MessageBoxDefaultButton::Button2); return res==::DialogResult::Yes; }
void UpdaterPopup::DoUpdateForRelease(GitHubRelease^ release){ if(release==nullptr) return; if(!ConfirmDowngradeIfNeeded(release)) return; ReleaseAsset^ asset=release->GetAuthoritativeInstallerAsset(); if(asset==nullptr){ MessageBox::Show(this,"This release has no installer asset (Windows_Hello_Fix_Setup.exe).\n\nOpen on GitHub to download manually.","Updater",MessageBoxButtons::OK,MessageBoxIcon::Information); return; } if(!UpdateInstaller::IsValidGithubAssetUrl(asset->BrowserDownloadUrl)){ MessageBox::Show(this,"Invalid asset URL. Update blocked for security.","Updater",MessageBoxButtons::OK,MessageBoxIcon::Error); return; } auto confirm=MessageBox::Show(this, String::Format("Update to {0}?\n\nThe installer will be downloaded to a temporary location, verified, and launched.\nThe app will exit and restart at the new version.\n\nDownload size: {1} bytes\nFrom: {2}", release->Version->ToDisplayString(), asset->Size, asset->BrowserDownloadUrl), "Confirm Update", MessageBoxButtons::YesNo, MessageBoxIcon::Question); if(confirm!=::DialogResult::Yes) return; pendingUpdateRelease=release; auto task=updater->DownloadReleaseAsync(release); task->ContinueWith(gcnew Action<Task<bool>^>(this,&UpdaterPopup::OnDownloadTaskCompleted), TaskScheduler::Default); }
void UpdaterPopup::OnDownloadTaskCompleted(Task<bool>^ t){ try{ bool ok=false; if(t!=nullptr&&t->IsCompleted&&!t->IsFaulted&&!t->IsCanceled) ok=t->Result; if(!ok){ if(this->InvokeRequired) this->BeginInvoke(gcnew Action<String^>(this,&UpdaterPopup::DoUpdateForRelease_Failed), "Download failed. See status banner."); else DoUpdateForRelease_Failed("Download failed."); return; } GitHubRelease^ rel=pendingUpdateRelease; if(this->InvokeRequired) this->BeginInvoke(gcnew Action<GitHubRelease^>(this,&UpdaterPopup::DoUpdateForRelease_Launch), rel); else DoUpdateForRelease_Launch(rel); }catch(Exception^ ex){ if(this->InvokeRequired) this->BeginInvoke(gcnew Action<String^>(this,&UpdaterPopup::DoUpdateForRelease_Failed), ex->Message); else DoUpdateForRelease_Failed(ex->Message); } }
void UpdaterPopup::DoUpdateForRelease_Failed(String^ msg){ MessageBox::Show(this,msg,"Updater",MessageBoxButtons::OK,MessageBoxIcon::Error); }
void UpdaterPopup::DoUpdateForRelease_Launch(GitHubRelease^){ auto res=MessageBox::Show(this,"Download verified. Launch installer now?\n\nThe app will exit.","Updater",MessageBoxButtons::YesNo,MessageBoxIcon::Question); if(res!=::DialogResult::Yes) return; bool ok=updater->LaunchStagedInstaller(false); if(!ok){ MessageBox::Show(this,"Failed to launch installer. You can try again or use 'Download' to save manually.","Updater",MessageBoxButtons::OK,MessageBoxIcon::Error); } }
void UpdaterPopup::OpenUrl(String^ url){ if(String::IsNullOrEmpty(url)) return; Uri^ uri; if(!Uri::TryCreate(url,UriKind::Absolute,uri)) return; if(uri->Scheme!="https") return; if(!uri->Host->Equals("github.com",StringComparison::OrdinalIgnoreCase)){ MessageBox::Show(this,"URL not allowed: "+url,"Updater",MessageBoxButtons::OK,MessageBoxIcon::Warning); return; } try{ ProcessStartInfo^ psi=gcnew ProcessStartInfo(); psi->FileName=url; psi->UseShellExecute=true; Process::Start(psi); }catch(Exception^ ex){ MessageBox::Show(this,"Failed to open browser: "+ex->Message,"Updater",MessageBoxButtons::OK,MessageBoxIcon::Error); } }
} }
