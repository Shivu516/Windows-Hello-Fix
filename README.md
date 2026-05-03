# Windows Hello Camera Fix 📸⚡

A lightweight, highly optimized hardware-level fix for laptops struggling with Windows Hello Face Unlock conflicts between RGB and IR camera sensors. 

## 🛑 The Problem

On many prosumer laptops and 2-in-1s, Windows Hello struggles to authenticate quickly (or at all) because the system's RGB (color) camera and IR (infrared) sensor fight for priority. When waking from sleep or locking the screen, Windows often tries to use the slower RGB sensor for facial recognition or gets confused comparing IR data against an RGB-based biometric template. This results in slow unlocks, "looking for you" loops, and failed attempts.

## 💡 The Solution & "The IR Trick" (Crucial)

This project fixes the issue through a two-part methodology: an **automated hardware toggle** and a **biometric enrollment trick**.

### 1. The Pure IR Biometric Template
Through extensive testing, we discovered that if you disable the RGB camera *before* setting up Face Unlock, Windows is forced to create a **Pure IR Biometric Template**. 
Even if the RGB camera is re-enabled later, Windows will prioritize the IR sensor for future scans and recognition improvements, resulting in a significantly faster and flawless unlock experience.

### 2. The Automated Hardware Toggle
To ensure 0% interference and save battery, this script automatically disables the RGB camera the millisecond you lock your workstation, and re-enables it the millisecond you unlock it. This ensures Windows Hello only ever sees the IR sensor when it needs to scan your face.

## 🚀 Under the Hood: Why this is Blazing Fast

You might be wondering why we don't just use standard Task Scheduler with PowerShell scripts. The answer is **Race Conditions**. Windows Hello initiates the camera in about 0.5 seconds. Standard scripts take ~1.5 seconds to load, meaning the camera is already active before the script can disable it. 

This project uses extreme optimizations to beat Windows Hello to the punch:

* **Kernel-Level Event ID Triggers:** Instead of relying on sluggish Task Scheduler "Session State" polling, this setup utilizes `auditpol.exe` to enforce Security Log auditing. It hooks directly into **Event ID 4800 (Workstation Lock)** and **Event ID 4801 (Workstation Unlock)** for instantaneous triggering.
* **Zero-Overhead Execution (`pnputil`):** We completely bypass PowerShell `.ps1` execution delays. The scheduled tasks trigger featherweight `.vbs` wrappers that execute native Windows `pnputil.exe` commands. This talks directly to the Plug-and-Play kernel manager, executing the hardware toggle in milliseconds.

## 🛠️ Installation & Setup Guide

**Step 1: Install the Fix**
1. Download or clone this repository to your local machine.
2. Right-click `install.bat` and select **Run as Administrator**.
3. A GUI window will appear. Select your **RGB Camera** from the dropdown menu (this is the camera you want to temporarily disable during lock screens).
4. Click **Apply Fix**. The script will automatically generate the optimized files, configure your security audit policies, and register the high-speed tasks.

**Step 2: Apply the "IR Trick" (Required for best results)**
1. Once installed, **Lock your PC** (`Win + L`) and unlock it once to ensure the RGB camera is disabled.
2. Go to Windows **Settings > Accounts > Sign-in options > Facial recognition (Windows Hello)**.
3. Click **Remove** to delete your current messy face data.
4. Click **Set up** and re-enroll your face. 
*(Because the script has turned off your RGB camera, Windows will now build a flawless, Pure IR template!)*

## 🧹 Uninstallation

Want to revert to standard Windows behavior?
1. Right-click `uninstall.bat` and select **Run as Administrator**.
2. The script will safely remove all scheduled tasks and delete the generated system folders. Your PC will be returned to its default state.

## ⚠️ Disclaimer

This tool directly modifies hardware states using built-in Windows utilities. While perfectly safe and easily reversible, ensure you are selecting the correct RGB camera during setup. Do not select your IR sensor, or Windows Hello will stop working entirely!
