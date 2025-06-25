# Contributing to AutoWatering

Thank you for your interest in contributing to the AutoWatering project! This guide provides information on how to contribute effectively.

## Code of Conduct

By participating in this project, you agree to abide by our code of conduct:

- Be respectful and inclusive
- Focus on constructive feedback
- Resolve disagreements professionally
- Put the project's best interests first
- Maintain a harassment-free environment for everyone

## How to Contribute

There are many ways to contribute to AutoWatering:

1. **Code Contributions**: Add new features or fix bugs
2. **Documentation**: Improve or expand documentation and examples
3. **Testing**: Test the system and report issues
4. **Feature Suggestions**: Propose new features or enhancements
5. **Bug Reports**: Report issues you find while using the system

## Getting Started

### Development Setup

1. **Fork the Repository**:
   - Fork the project on GitHub
   - Clone your fork locally:
   ```bash
   git clone https://github.com/<your-username>/AutoWatering.git
   cd AutoWatering
   ```

2. **Set Up Development Environment**:
   - Follow the [Installation Guide](INSTALLATION.md)
   - Make sure you can successfully build and flash the project

3. **Create a Branch**:
   ```bash
   git checkout -b feature/my-feature
   # or
   git checkout -b fix/my-fix
   # or  
   git checkout -b docs/update-documentation
   ```

### Hardware Testing

If contributing hardware-related changes:

1. **Test on Real Hardware**:
   - Verify changes work on actual nRF52840 hardware
   - Test with real solenoid valves and flow sensors
   - Document any new hardware requirements

2. **Power Consumption**:
   - Measure power consumption for new features
   - Ensure changes don't significantly impact battery life
   - Document power requirements

3. **Bluetooth Compatibility**:
   - Test with multiple client platforms (Android, iOS, Web)
   - Verify MTU handling and fragmentation
   - Check notification performance

### Coding Standards

Follow these standards when writing code:

1. **Zephyr Coding Style**:
   - Follow the [Zephyr Coding Style Guidelines](https://docs.zephyrproject.org/latest/contribute/coding_guidelines/index.html)
   - Use 4-space indentation (not tabs)
   - Maximum line length of 100 characters

2. **Documentation**:
   - Add Doxygen comments to all functions, structures, and enums
   - Keep comments updated if you modify code
   - Example:
   ```c
   /**
    * @brief Function description
    *
    * Detailed description of function purpose and operation.
    *
    * @param param1 Description of parameter 1
    * @param param2 Description of parameter 2
    *
    * @return Description of return value
    */
   ```

3. **Error Handling**:
   - Use the defined `watering_error_t` error codes consistently
   - Check all API returns and handle errors appropriately
   - No silent failures

4. **Resource Management**:
   - Clean up all resources in error paths
   - Use the RAII pattern where possible
   - No memory leaks

5. **Thread Safety**:
   - Use appropriate synchronization primitives (mutexes, semaphores)
   - Document thread safety expectations in header files

### Pull Request Process

1. **Create a Pull Request**:
   - Push your branch to your fork:
   ```bash
   git push origin feature/my-feature
   ```
   - Go to GitHub and create a pull request against the main repository

2. **PR Description**:
   - Describe what your changes do and why
   - Link to any related issues
   - Include test results or screenshots if relevant
   
3. **Code Review**:
   - All PRs must be reviewed by at least one maintainer
   - Address reviewer feedback promptly
   - Maintainers may request changes before merging

4. **Continuous Integration**:
   - Wait for CI checks to complete successfully
   - Fix any issues raised by automated tests

5. **Merging**:
   - Maintainers will merge your PR when it's ready
   - You may be asked to rebase if there are conflicts

## Development Guidelines

### Feature Development

When implementing new features:

1. **Discuss First**:
   - Open an issue to discuss the feature before implementing
   - Get consensus on the approach and design

2. **Minimal Changes**:
   - Keep changes focused and minimal
   - Split large features into smaller, incremental PRs

3. **Tests**:
   - Add tests for new functionality where possible
   - Ensure existing tests continue to pass
   - Test with real hardware when applicable

### Bluetooth API Changes

Special considerations for Bluetooth interface modifications:

1. **Backward Compatibility**:
   - Maintain compatibility with existing clients when possible
   - Document breaking changes clearly in BLUETOOTH.md
   - Provide migration guidance for API changes

2. **Structure Packing**:
   - Ensure all structures use `__packed` attribute
   - Verify byte alignment matches documentation
   - Test on different platforms for endianness issues

3. **MTU Considerations**:
   - Test with default MTU (23 bytes) for web browser compatibility
   - Verify fragmentation protocols work correctly
   - Document structure size requirements clearly

4. **Testing Requirements**:
   - Test with multiple client platforms (Android, iOS, Web)
   - Verify notification behavior and timing
   - Check error handling and recovery

### Bug Fixes

When fixing bugs:

1. **Reproduce First**:
   - Make sure you can reproduce the issue
   - Create a test case that demonstrates the bug

2. **Fix the Root Cause**:
   - Address the root cause, not just symptoms
   - Consider if similar bugs might exist elsewhere

3. **Documentation**:
   - Update documentation if the bug was related to unclear docs
   - Add comments explaining non-obvious fixes

### Documentation

When updating documentation:

1. **Clear Language**:
   - Use simple, clear language
   - Avoid jargon where possible
   - Consider non-native English speakers

2. **Examples**:
   - Include practical examples for new features
   - Update examples to match code changes

3. **Formatting**:
   - Use Markdown formatting consistently
   - Check rendering before submitting

## Development Workflow

1. **Issue Tracking**:
   - All work should be linked to a GitHub issue
   - Use issue numbers in commit messages and PR titles

2. **Branching**:
   - Feature branches: `feature/description`
   - Bug fix branches: `fix/issue-description`
   - Release branches: `release/version`

3. **Commit Messages**:
   - Use clear, descriptive commit messages
   - Format: `[Component] Brief description (max 50 chars)`
   - Add detailed description in commit body if needed

4. **Version Control**:
   - Make atomic commits (one logical change per commit)
   - Rebase branches before creating PRs
   - Avoid merge commits when possible

## Releasing

The release process is managed by maintainers:

1. **Version Numbers**:
   - We follow [Semantic Versioning](https://semver.org/)
   - Format: MAJOR.MINOR.PATCH

2. **Release Notes**:
   - All significant changes must be documented
   - Group changes by type: Features, Bug Fixes, etc.

3. **Release Approval**:
   - Releases require approval by at least two maintainers
   - All tests must pass on the release branch

## Getting Help

If you need help with your contribution:

- Ask in the related issue
- Contact the project maintainers
- Check the [Troubleshooting Guide](TROUBLESHOOTING.md)

Thank you for contributing to AutoWatering!

[Back to main README](../README.md)
