<div align="center">

<img src="assets/logo.png" alt="AkiraOS Logo" width="250"/>

</div>

# Contributing to AkiraOS

🎮 Thank you for your interest in contributing to AkiraOS! We welcome contributions from developers of all skill levels.

## 📋 Table of Contents

- [Code of Conduct](#code-of-conduct)
- [Getting Started](#getting-started)
- [Development Setup](#development-setup)
- [How to Contribute](#how-to-contribute)
- [Coding Standards](#coding-standards)
- [Commit Guidelines](#commit-guidelines)
- [Pull Request Process](#pull-request-process)
- [Testing](#testing)
- [Documentation](#documentation)
- [Community](#community)

## 📜 Code of Conduct

This project adheres to our [Code of Conduct](CODE_OF_CONDUCT.md). By participating, you are expected to uphold this code. Please report unacceptable behavior to support@pen.engineering.

## 🚀 Getting Started

### Prerequisites

Before contributing, ensure you have:
- Zephyr SDK installed and configured
- ESP-IDF tools for ESP32 development  
- Python 3.8+ with required packages
- Git configured with your GitHub account
- Basic knowledge of C/C++ and embedded systems

### First Time Setup

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:
   ```bash
   git clone https://github.com/YOUR-USERNAME/AkiraOS.git
   cd AkiraOS
   ```
3. **Add upstream remote**:
   ```bash
   git remote add upstream https://github.com/ArturR0k3r/AkiraOS.git
   ```
4. **Follow the setup instructions** in the main README.md

## 🛠️ Development Setup

### Environment Configuration

1. **Initialize the workspace**:
   ```bash
   west init -l .
   west update
   west blobs fetch hal_espressif
   ```

2. **Verify build setup**:
   ```bash
   ./build.sh          # builds and runs native_sim
   ```

3. **Test on hardware** (if available):
   ```bash
   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
   ```

## 🤝 How to Contribute

### Types of Contributions

We welcome various types of contributions:

- 🐛 **Bug fixes** - Fix existing issues
- ✨ **New features** - Add functionality to AkiraOS
- 📚 **Documentation** - Improve docs, tutorials, examples
- 🎮 **Games/Apps** - Create WASM applications
- 🧪 **Testing** - Add tests, improve test coverage
- 🎨 **UI/UX** - Enhance visual design and user experience
- 🔧 **Tools** - Build scripts, development tools

### Finding Work

- Check [GitHub Issues](https://github.com/ArturR0k3r/AkiraOS/issues) for open tasks
- Look for `good first issue` labels for beginner-friendly tasks
- Browse `help wanted` labels for areas needing assistance
- Join discussions in [GitHub Discussions](https://github.com/ArturR0k3r/AkiraOS/discussions)

### Product Branches

Product firmware (KeyaPlatform, AkiraEar, Latch) has lived on long-lived
branches that patch core files. Those branches are **frozen**: they get no
merges from the active release branch, and new core work must not start on them.

- Generic drivers and fixes found on a product branch come back to core as
  normal pull requests against the active release branch.
- New product work keeps product-only code behind its own Kconfig symbol and
  out of shared core files, so it can move to its own repository once AkiraOS
  can be consumed as a Zephyr module.

## 📝 Coding Standards

### C/C++ Code Style

Follow the [Zephyr Project Coding Guidelines](https://docs.zephyrproject.org/latest/contribute/guidelines.html#coding-style):

```c
// Good: Clear function names, proper spacing
static int akira_display_init(const struct device *dev) 
{
    if (!device_is_ready(dev)) {
        return -ENODEV;
    }
    
    return 0;
}

// Good: Consistent naming convention
#define AKIRA_DISPLAY_WIDTH     240
#define AKIRA_DISPLAY_HEIGHT    320

// Good: Structured error handling
enum akira_error {
    AKIRA_OK = 0,
    AKIRA_ERROR_INVALID_PARAM = -1,
    AKIRA_ERROR_NO_MEMORY = -2,
};
```

### General Guidelines

- **Indentation**: Use 4 spaces (no tabs)
- **Line Length**: Maximum 100 characters
- **Naming**: Use descriptive names with `akira_` prefix for public APIs
- **Comments**: Document complex logic and public functions
- **Error Handling**: Always check return values and handle errors gracefully

### File Structure

```
src/
├── main.c          # Entry point
├── runtime/        # WAMR integration and app lifecycle
├── api/            # Native API wrappers (display, GPIO, BLE, etc.)
├── connectivity/   # WiFi, BLE, OTA, HTTP server
├── drivers/        # HAL and hardware drivers
├── shell/          # Interactive CLI commands
├── ui/             # Native UI framework
└── lib/            # Shared utilities
```

## 📝 Commit Guidelines

### Commit Message Format

Use the [Conventional Commits](https://www.conventionalcommits.org/) specification:

```
<type>(<scope>): <description>

[optional body]

[optional footer]
```

### Types

- `feat`: New feature
- `fix`: Bug fix
- `docs`: Documentation changes
- `style`: Code style changes (formatting, etc.)
- `refactor`: Code refactoring
- `test`: Adding or updating tests
- `chore`: Maintenance tasks

### Examples

```bash
feat(display): add support for 16-bit color mode

fix(input): resolve button debouncing issue

docs(api): update WASM development guide

test(core): add unit tests for memory management
```

## 🔄 Pull Request Process

### Before Submitting

1. **Sync with upstream**:
   ```bash
   git fetch upstream
   git checkout main
   git merge upstream/main
   ```

2. **Create a feature branch**:
   ```bash
   git checkout -b feature/your-feature-name
   ```

3. **Make your changes** following coding standards

4. **Test your changes**:
   ```bash
   # Test on native_sim first
   ./build.sh
   # Flash to hardware if available
   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
   ```

5. **Commit your changes**:
   ```bash
   git add .
   git commit -m "feat(scope): description of changes"
   ```

### Submitting the PR

1. **Push to your fork**:
   ```bash
   git push origin feature/your-feature-name
   ```

2. **Create Pull Request** on GitHub with:
   - Clear title and description
   - Reference related issues
   - Include screenshots/videos for UI changes
   - List any breaking changes

### PR Requirements

- [ ] Code follows project coding standards
- [ ] Changes are tested (on hardware if possible)
- [ ] Documentation is updated if needed
- [ ] Commit messages follow convention
- [ ] PR description is clear and complete
- [ ] No merge conflicts with main branch

## 🧪 Testing

### Hardware Testing

If you have AkiraOS hardware:

1. **Flash your changes**:
   ```bash
   ./build.sh -b esp32s3_devkitm_esp32s3_procpu -r a
   ```

2. **Test basic functionality**:
   - Boot sequence
   - Menu navigation
   - Button responsiveness
   - Display rendering

3. **Test specific features** you've modified

### Simulation Testing

For testing without hardware:
- Use Zephyr's native POSIX simulator when possible
- Verify code compiles for ESP32 target
- Run static analysis tools

## 📖 Documentation

### When to Update Documentation

Update documentation when you:
- Add new APIs or change existing ones
- Create new features or applications
- Fix bugs that affect user experience
- Add new build/setup requirements

### Documentation Types

- **API Reference**: Document all public functions
- **Tutorials**: Step-by-step guides for developers
- **Hardware Guide**: Assembly and setup instructions
- **Troubleshooting**: Common issues and solutions

## 💬 Community

### Getting Help

- **GitHub Discussions**: For questions and general discussion
- **GitHub Issues**: For bug reports and feature requests
- **Email**: support@pen.engineering for direct support

### Communication Guidelines

- Be respectful and inclusive
- Search existing issues/discussions before posting
- Provide clear, detailed information
- Use English for all communications
- Be patient - maintainers are volunteers

## 🎯 Areas of Focus

We're particularly looking for contributions in:

- **WASM Runtime**: Improving performance and capabilities
- **Hardware Drivers**: Supporting additional peripherals
- **Games & Applications**: Building the software ecosystem
- **Documentation**: Making the project more accessible
- **Testing**: Increasing test coverage and reliability
- **Hacker Mode**: Implementing cybersecurity tools

## 📄 License

By contributing to AkiraOS, you agree that your contributions will be licensed under the same [Apache License 2.0](LICENSE) that covers the project.
