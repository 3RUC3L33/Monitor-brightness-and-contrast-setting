# Bright Controller v1.0

Bright Controller is a lightweight Windows utility that allows you to easily control your monitors' brightness and contrast directly from your desktop. No more awkward fumbling with physical monitor OSD (On-Screen Display) buttons—manage everything seamlessly via software.

<p align="center">
  <a href="https://githubusercontent.com" target="_blank">
    <img src="https://github.com/3RUC3L33/Monitor-brightness-and-contrast-setting/screenshot.jpg" alt="Bright Controller App Preview" width="600" style="max-width: 100%;">
  </a>
</p>
## 🚀 Features

*   **OSD-Free Control:** Adjust brightness and contrast without touching your physical monitor buttons.
*   **Multi-Monitor Support:** Detects and manages multiple displays independently or simultaneously.
*   **Windows Theme Integration:** Automatically detects your Windows Light/Dark mode theme settings.
*   **Persistent Settings:** Your preferred brightness and contrast levels are automatically saved and restored on launch.
*   **Lightweight & Fast:** Runs quietly in the background with minimal system resource footprint.

## 🛠️ Requirements

*   **Operating System:** Windows 10 or Windows 11
*   **Hardware Feature:** Your monitors must have **DDC/CI** (Display Data Channel / Command Interface) enabled in their physical OSD settings for software control to work.

## 📦 Installation

1. Go to the [Releases](https://github.com) page.
2. Download the latest version of `BrightController.exe` (or the provided installer).
3. Run the application. 

## 🔧 How It Works

The app utilizes the **DDC/CI** protocol to communicate directly with your graphics card and connected displays. By sending VCP (Virtual Control Panel) feature codes, it adjusts internal monitor parameters instantly. It also listens to Windows registry changes to dynamically respond to system theme switches.

## 🤝 Contributing

Contributions are welcome! If you'd like to improve Bright Controller:
1. Fork the repository.
2. Create a feature branch (`git checkout -b feature/AmazingFeature`).
3. Commit your changes (`git commit -m 'Add some AmazingFeature'`).
4. Push to the branch (`git push origin feature/AmazingFeature`).
5. Open a Pull Request.

## 📝 License

Distributed under the MIT License. See `LICENSE` for more information.
