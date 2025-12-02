Fork of https://github.com/ThePagi/PBRNifPatcher with a few additions:

- Added detailed logging (info/warn/error) to console and `pbr_patcher.log` in the executable directory.
- Per-file error isolation: NIF processing exceptions or code-page issues are logged and skipped instead of stopping the run.
- GitHub Action to build Release x64 on Windows (MSBuild) and publish the artifact.

Usage and configs follow the original PBRNifPatcher behavior.***
