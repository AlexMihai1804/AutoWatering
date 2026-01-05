# Contributing to AutoWatering

Thank you for your interest in contributing to AutoWatering! This document provides guidelines for contributing to the project.

## Getting Started

1. **Fork the repository** on GitHub
2. **Clone your fork** locally:

   ```bash
   git clone https://github.com/YOUR-USERNAME/AutoWatering.git
   cd AutoWatering
   ```

3. **Set up the development environment** following [docs/INSTALLATION.md](docs/INSTALLATION.md)

## Development Workflow

### Branching Strategy

- `main` - stable release branch
- Feature branches should be named: `feature/short-description`
- Bug fix branches should be named: `fix/issue-number-description`

### Making Changes

1. Create a new branch from `main`:

   ```bash
   git checkout -b feature/your-feature-name
   ```

2. Make your changes following the coding conventions below
3. Test your changes on the target hardware (`arduino_nano_33_ble`)
4. Commit with clear, descriptive messages (see Commit Guidelines)

### Commit Guidelines

Follow [Conventional Commits](https://www.conventionalcommits.org/):

- `feat:` - New features
- `fix:` - Bug fixes
- `docs:` - Documentation changes
- `chore:` - Maintenance tasks
- `refactor:` - Code refactoring without functional changes

Examples:

```text
feat(ble): add new characteristic for soil moisture config
fix(watering): correct FAO-56 calculation for dense crops
docs: update BLE API documentation for characteristic 28
chore: update Zephyr version to 4.3.0
```

### Pull Request Process

1. Ensure your code builds without warnings:

   ```bash
   west build -b arduino_nano_33_ble --pristine
   ```

2. Update documentation if you've changed behavior or added features
3. Update `docs/CHANGELOG.md` with your changes
4. Submit a pull request with a clear description of changes

## Coding Conventions

### C Code Style

- Use Zephyr's coding style (Linux kernel style with modifications)
- 8-character tabs for indentation
- Maximum line length: 100 characters
- Use `LOG_*` macros for logging, not `printk` (except early init)
- Return `watering_error_t` or Zephyr `-errno` for error handling

### File Organization

- Source files in `src/`
- Board overlays in `boards/`
- Documentation in `docs/`
- Generated database files (`.inc`) should not be edited manually - use `tools/build_database.py`

### BLE Characteristic Changes

When modifying BLE characteristics:

1. Update `src/bt_irrigation_service.c` or `src/bt_custom_soil_handlers.c`
2. Update `ATTR_IDX_*` constants if adding/removing characteristics
3. Update corresponding documentation in `docs/ble-api/characteristics/`
4. Update `docs/ble-api/README.md` characteristic table
5. Increment `BLE_IRRIGATION_VERSION` in `src/bt_irrigation_service.h`

## Reporting Issues

When reporting bugs, please include:

- Firmware version (from boot log or BLE Device Information Service)
- Hardware configuration
- Steps to reproduce
- Expected vs actual behavior
- Relevant log output

## Questions?

Open a [GitHub Discussion](https://github.com/AlexMihai1804/AutoWatering/discussions) for questions or ideas.

## License

By contributing, you agree that your contributions will be licensed under the MIT License.
