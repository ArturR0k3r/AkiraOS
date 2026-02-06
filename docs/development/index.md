# Development Guide

Advanced topics for AkiraOS development.

## Topics

### Building Applications
- [Building WASM Apps](building-apps.md) - Complete WASM development workflow
- [OTA Updates](ota-updates.md) - Over-the-air firmware deployment
- [Debugging](debugging.md) - Debug techniques and tools

### Contributing
See [CONTRIBUTING.md](../../CONTRIBUTING.md) for contribution guidelines.

## Quick Links

- **Build System:** West + CMake
- **Source Code:** `/src` directory
- **Tests:** `/tests` directory
- **Examples:** `/wasm_sample` directory

## Development Workflow

1. Make code changes
2. Build firmware (`./build.sh`)
3. Flash to hardware (`west flash`)
4. Test functionality
5. Commit changes

## Related Documentation

- [Architecture](../architecture/) - System design
- [API Reference](../api-reference/) - Native APIs
- [Troubleshooting](../getting-started/troubleshooting.md) - Common issues
