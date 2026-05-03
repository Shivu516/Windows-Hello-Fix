# Windows Hello Camera Fix 📸⚡

A lightweight, highly optimised hardware-level fix for laptops struggling with Windows Hello Face Unlock conflicts between RGB and IR camera sensors. 

## 🛑 The Problem

On many prosumer laptops and 2-in-1s, Windows Hello struggles to authenticate quickly (or at all) because the system's RGB (colour) camera and IR (infrared) sensor fight for priority. When waking from sleep or locking the screen, Windows often tries to use the slower RGB sensor for facial recognition or gets confused comparing IR data against an RGB-based biometric template. This results in slow unlocks, "looking for you" loops, and failed attempts.

## 💡 The Solution & "The IR Trick" (Crucial)

This project fixes the issue through a two-part methodology: an **automated hardware toggle** and a **biometric enrollment trick**.

### 1. The Pure IR Biometric Template
Through extensive testing, we discovered that if you disable the RGB camera *before* setting up Face Unlock, Windows is forced to create a **Pure IR Biometric Template**. 
Even if the RGB camera is re-enabled later, Windows will prioritise the IR sensor for future scans and recognition improvements, resulting in a significantly faster and flawless unlock experience.

### 2. The Automated Hardware Toggle
To ensure 0% interference and save battery, this script automatically disables the RGB camera the millisecond you lock your workstation, and re-enables it the millisecond you unlock it. This ensures Windows Hello only ever sees the IR sensor when it needs to scan your face.

## 🚀 Under the Hood: Why this is Blazing Fast

You might be wondering why we don't just use the standard Task Scheduler with PowerShell scripts. The answer is **Race Conditions**. Windows Hello initiates the camera in about 0.5 seconds. Standard scripts take ~1.5 seconds to load, meaning the camera is already active before the script can disable it. 

This project uses extreme optimisations to beat Windows Hello to the punch:

* **Kernel-Level Event ID Triggers:** Instead of relying on sluggish Task Scheduler "Session State" polling, this setup utilises `auditpol.exe` to enforce Security Log auditing. It hooks directly into **Event ID 4800 (Workstation Lock)** and **Event ID 4801 (Workstation Unlock)** for instantaneous triggering.
* **Zero-Overhead Execution (`pnputil`):** We completely bypass PowerShell `.ps1` execution delays. The scheduled tasks trigger featherweight `.vbs` wrappers that execute native Windows `pnputil.exe` commands. This talks directly to the Plug-and-Play kernel manager, executing the hardware toggle in milliseconds.

## 🛠️ Installation & Setup Guide

**Step 1: Apply the "Pure IR" Trick (Crucial First Step)**
Before installing the script, we need to force Windows to build a flawless infrared biometric template.
 1. Open **Device Manager** (Right-click the Start button or press Win + X and select it).
 2. Expand the **Cameras** (or Imaging devices) category.
 3. Find your standard **RGB Camera**, right-click it, and select **Disable device**.
 4. **Sign out** or lock your PC (Win + L), then sign back in using your PIN.
 5. Go to Windows **Settings > Accounts > Sign-in options > Facial recognition (Windows Hello)**.
 6. Click **Remove** to delete your old, conflicting face data.
 7. Click **Set up** and re-enroll your face.
   *(Pro Tip: If you see a black-and-white video feed during setup, it worked! Windows is now generating a highly reliable, Pure IR template.)*
 8. Once finished, go back to Device Manager, right-click your RGB camera, and select **Enable device** so you can still use it for video calls.
    
**Step 2: Install the Hardware Toggle (The Script)**
Now that you have a perfect IR base, we need to install the script to act as a shield. This prevents unnecessary conflicts where Windows tries to force the RGB camera to wake up during lock screens.
 1. Download the `Windows Hello Fix` file.
 2. Right-click install.bat and select **Run as Administrator**.
 3. A GUI window will appear. Select your **RGB Camera** from the dropdown menu (this tells the script which camera to temporarily kill during lock screens).
 4. Click **Apply Fix**. The script will automatically generate the optimised files, configure your security audit policies, and register the high-speed background tasks.


## 🧹 Uninstallation

Want to revert to standard Windows behaviour?
1. Right-click `uninstall.bat` and select **Run as Administrator**.
2. The script will safely remove all scheduled tasks and delete the generated system folders. Your PC will be returned to its default state.

## ⚠️ Disclaimer

This tool directly modifies hardware states using built-in Windows utilities. While perfectly safe and easily reversible, ensure you are selecting the correct RGB camera during setup. Do not select your IR sensor, or Windows Hello will stop working entirely!

## Keywords for Search Engines
Windows Hello face recognition slow fix, prioritise IR camera over RGB Windows 11, Windows Hello looking for you loop, disable RGB camera for face unlock, pnputil camera toggle script, Event ID 4800 4801 task scheduler fix, infrared camera biometric enrollment trick, Windows Hello hardware conflict solution.
