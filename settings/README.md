# Global settings

This directory owns optional non-app-specific settings. App-specific settings
remain under `apps/<app>/settings/`.

The core settings implementation contains only generic storage, validation,
UI, and registry infrastructure. Individual setting modules live in this
directory and are selected by profiles.
