# Get a ready-to-run Windows ZIP without compiling on your PC

This repository includes `.github/workflows/windows-release.yml`. GitHub Actions builds RPK on a hosted Windows 2022 runner, runs the test suite, and uploads `RPK-Windows-x64.zip` as an artifact. Your Windows PC does not need Visual Studio, CMake, vcpkg, or a local C++ build for this route.

## One-time setup

1. Create a new **private** repository on GitHub (for example, `rpk-compressor`).
2. Upload the contents of this project folder to the repository, including the hidden `.github` folder and its `workflows/windows-release.yml` file. The easiest reliable method is Git from PowerShell:

   ```powershell
   cd path\to\RPK_M4_Windows_AutoSetup
   git init
   git add .
   git commit -m "Add RPK source and Windows release workflow"
   git branch -M main
   git remote add origin https://github.com/YOUR-ACCOUNT/rpk-compressor.git
   git push -u origin main
   ```

3. Open the repository on GitHub and select **Actions**. Enable Actions for the repository if GitHub asks.
4. Select **Build Windows Release** and click **Run workflow**.
5. When the run completes, open it and download the **RPK-Windows-x64** artifact.
6. Extract the ZIP and double-click `rpk_gui.exe`. `rpk.exe` is the CLI.

The workflow is configured to link the compression libraries statically. It also copies any runtime DLLs that CMake places beside the application and includes SHA-256 checksums.

## If the workflow fails

Open the failed workflow run, expand the first red step, and share its complete log. The workflow uses a standard Windows 2022 runner with Visual Studio 2022, then bootstraps vcpkg and installs the four native dependencies.

## Alternative: automatic setup on your own PC

Double-click `Install-RPK.cmd` from the source folder. It will configure and compile locally, then install shortcuts. This route needs Visual Studio C++ tools, CMake, Git, and internet access. Use the GitHub Actions route above when you do not want to compile locally.
