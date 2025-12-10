#!/usr/bin/env bash
#
# deps-manage.sh - Universal Dependency Manager CLI
# =================================================
#
# A command-line tool for managing CMake dependencies across projects.
# Works with the DepsManager.cmake module.
#
# USAGE:
#   ./deps-manage.sh <command> [options]
#
# COMMANDS:
#   status              Show status of all dependencies
#   local <dep> <path>  Set up local development for a dependency
#   unlocal <dep>       Remove local override for a dependency
#   bundle [opts]       Create offline source bundle
#   init                Initialize deps directory structure
#   sync                Clone/update all dependencies to local
#
# OPTIONS:
#   -c, --config FILE   Deps configuration file (default: deps/deps.cmake)
#   -d, --deps-dir DIR  Dependencies directory (default: deps/)
#   -q, --quiet         Suppress output
#   -h, --help          Show this help
#
# EXAMPLES:
#   # Show dependency status
#   ./deps-manage.sh status
#
#   # Set up varint for local development (creates symlink)
#   ./deps-manage.sh local varint ~/repos/varint
#
#   # Create offline bundle
#   ./deps-manage.sh bundle -o releases/
#
#   # Clone all deps for local development
#   ./deps-manage.sh sync

set -euo pipefail

# Colors
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
CYAN='\033[0;36m'
BOLD='\033[1m'
NC='\033[0m'

# Defaults
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
DEPS_DIR="${PROJECT_ROOT}/deps"
CONFIG_FILE="${DEPS_DIR}/deps.cmake"
LOCAL_FILE="${DEPS_DIR}/.local.cmake"
QUIET=false

# ============================================================================
# Utility Functions
# ============================================================================

log_info() { $QUIET || echo -e "${BLUE}[INFO]${NC} $1"; }
log_success() { $QUIET || echo -e "${GREEN}[OK]${NC} $1"; }
log_warn() { $QUIET || echo -e "${YELLOW}[WARN]${NC} $1"; }
log_error() { echo -e "${RED}[ERROR]${NC} $1" >&2; }

usage() {
    head -35 "$0" | grep -E "^#" | sed 's/^# \?//'
    exit 0
}

# Parse deps.cmake to extract dependency info
# Output format: name|git_url|tag
parse_deps_config() {
    local config_file="$1"

    if [[ ! -f "$config_file" ]]; then
        log_error "Config file not found: $config_file"
        exit 1
    fi

    # Extract deps_add calls and parse them
    # This is a simplified parser - handles basic cases
    local in_deps_add=false
    local current_name=""
    local current_git=""
    local current_tag="main"

    while IFS= read -r line; do
        # Remove comments
        line="${line%%#*}"

        # Check for deps_add start
        if [[ "$line" =~ deps_add\(([a-zA-Z0-9_-]+) ]]; then
            # Output previous if exists
            if [[ -n "$current_name" ]]; then
                echo "${current_name}|${current_git}|${current_tag}"
            fi
            current_name="${BASH_REMATCH[1]}"
            current_git=""
            current_tag="main"
            in_deps_add=true
        fi

        if $in_deps_add; then
            # Extract GIT url
            if [[ "$line" =~ GIT[[:space:]]+([^[:space:]]+) ]]; then
                current_git="${BASH_REMATCH[1]}"
            fi
            # Extract TAG
            if [[ "$line" =~ TAG[[:space:]]+([^[:space:]]+) ]]; then
                current_tag="${BASH_REMATCH[1]}"
            fi
            # Check for end of deps_add
            if [[ "$line" =~ \) ]]; then
                in_deps_add=false
            fi
        fi
    done < "$config_file"

    # Output last one
    if [[ -n "$current_name" ]]; then
        echo "${current_name}|${current_git}|${current_tag}"
    fi
}

# ============================================================================
# Commands
# ============================================================================

cmd_init() {
    log_info "Initializing dependency management structure..."

    mkdir -p "$DEPS_DIR"

    # Create deps.cmake template if it doesn't exist
    if [[ ! -f "$CONFIG_FILE" ]]; then
        cat > "$CONFIG_FILE" << 'EOF'
# deps.cmake - Dependency Configuration
# ======================================
#
# Declare your project's dependencies here.
# Use deps_add() for each dependency.
#
# Example:
#   deps_add(varint
#       GIT https://github.com/mattsta/varint.git
#       TAG v1.0.0
#       TARGETS varint-static
#   )
#
# See DepsManager.cmake for full documentation.

# --- Add your dependencies below ---

EOF
        log_success "Created $CONFIG_FILE"
    else
        log_info "Config file already exists: $CONFIG_FILE"
    fi

    # Create .local.cmake template
    if [[ ! -f "$LOCAL_FILE" ]]; then
        cat > "$LOCAL_FILE" << 'EOF'
# .local.cmake - Local Development Overrides
# ==========================================
#
# This file is gitignored. Use it to specify local paths for dependencies
# you're actively developing.
#
# Format:
#   set(DEPS_<NAME>_PATH "/path/to/local/checkout")
#
# Example:
#   set(DEPS_VARINT_PATH "$ENV{HOME}/repos/varint")
#
# You can also use environment variables:
#   export DEPS_VARINT_PATH=~/repos/varint

# --- Add your local overrides below ---

EOF
        log_success "Created $LOCAL_FILE"
    fi

    # Add .local.cmake to .gitignore if not already there
    local gitignore="${PROJECT_ROOT}/.gitignore"
    if [[ -f "$gitignore" ]]; then
        if ! grep -q "deps/.local.cmake" "$gitignore" 2>/dev/null; then
            echo -e "\n# Local dependency overrides\ndeps/.local.cmake" >> "$gitignore"
            log_success "Added .local.cmake to .gitignore"
        fi
    fi

    log_success "Initialization complete!"
    echo ""
    echo "Next steps:"
    echo "  1. Add dependencies to $CONFIG_FILE"
    echo "  2. Include DepsManager in your CMakeLists.txt:"
    echo ""
    echo "     include(cmake/deps-manager/DepsManager.cmake)"
    echo "     deps_init()"
    echo "     include(deps/deps.cmake)"
    echo "     deps_resolve()"
    echo ""
}

cmd_status() {
    log_info "Dependency Status"
    echo ""

    if [[ ! -f "$CONFIG_FILE" ]]; then
        log_error "No deps.cmake found. Run 'deps-manage.sh init' first."
        exit 1
    fi

    printf "%-20s %-8s %-50s\n" "DEPENDENCY" "MODE" "SOURCE"
    printf "%-20s %-8s %-50s\n" "----------" "----" "------"

    while IFS='|' read -r name git_url tag; do
        local mode="REMOTE"
        local source="${git_url}@${tag}"
        local name_upper="${name^^}"

        # Check for local sources
        if [[ -d "${DEPS_DIR}/${name}" ]]; then
            if [[ -L "${DEPS_DIR}/${name}" ]]; then
                mode="LOCAL*"  # Symlink
                source="$(readlink -f "${DEPS_DIR}/${name}")"
            else
                mode="LOCAL"
                source="${DEPS_DIR}/${name}"
            fi
        elif [[ -d "${PROJECT_ROOT}/vendor/${name}" ]]; then
            mode="VENDOR"
            source="${PROJECT_ROOT}/vendor/${name}"
        fi

        # Check .local.cmake for override
        if [[ -f "$LOCAL_FILE" ]]; then
            local local_path
            local_path=$(grep -E "set\(DEPS_${name_upper}_PATH" "$LOCAL_FILE" 2>/dev/null | \
                         sed -E 's/.*"([^"]+)".*/\1/' | head -1)
            if [[ -n "$local_path" ]]; then
                # Expand environment variables
                local_path=$(eval echo "$local_path")
                if [[ -d "$local_path" ]]; then
                    mode="LOCAL"
                    source="$local_path (from .local.cmake)"
                fi
            fi
        fi

        # Truncate source for display
        if [[ ${#source} -gt 50 ]]; then
            source="${source:0:47}..."
        fi

        printf "%-20s %-8s %-50s\n" "$name" "$mode" "$source"

    done < <(parse_deps_config "$CONFIG_FILE")

    echo ""
    echo "Legend: LOCAL* = symlink, LOCAL = directory, VENDOR = vendored, REMOTE = git"
}

cmd_local() {
    local dep_name="$1"
    local local_path="$2"

    if [[ -z "$dep_name" ]] || [[ -z "$local_path" ]]; then
        log_error "Usage: deps-manage.sh local <dependency> <path>"
        exit 1
    fi

    # Resolve to absolute path
    local_path="$(cd "$local_path" 2>/dev/null && pwd)" || {
        log_error "Path does not exist: $local_path"
        exit 1
    }

    if [[ ! -f "${local_path}/CMakeLists.txt" ]]; then
        log_error "No CMakeLists.txt found in: $local_path"
        exit 1
    fi

    local name_upper="${dep_name^^}"

    # Method 1: Create symlink in deps directory
    mkdir -p "$DEPS_DIR"
    if [[ -e "${DEPS_DIR}/${dep_name}" ]]; then
        log_warn "Removing existing ${DEPS_DIR}/${dep_name}"
        rm -rf "${DEPS_DIR}/${dep_name}"
    fi
    ln -s "$local_path" "${DEPS_DIR}/${dep_name}"
    log_success "Created symlink: ${DEPS_DIR}/${dep_name} -> ${local_path}"

    # Method 2: Also add to .local.cmake for explicit override
    if [[ -f "$LOCAL_FILE" ]]; then
        # Remove existing entry for this dep
        sed -i.bak "/DEPS_${name_upper}_PATH/d" "$LOCAL_FILE"
        rm -f "${LOCAL_FILE}.bak"
    else
        touch "$LOCAL_FILE"
    fi

    echo "set(DEPS_${name_upper}_PATH \"${local_path}\")" >> "$LOCAL_FILE"
    log_success "Added to .local.cmake: DEPS_${name_upper}_PATH"

    echo ""
    echo "Local development enabled for '${dep_name}'"
    echo "  - Changes in ${local_path} will be used directly"
    echo "  - Run 'cmake -B build' to reconfigure"
}

cmd_unlocal() {
    local dep_name="$1"

    if [[ -z "$dep_name" ]]; then
        log_error "Usage: deps-manage.sh unlocal <dependency>"
        exit 1
    fi

    local name_upper="${dep_name^^}"

    # Remove symlink
    if [[ -L "${DEPS_DIR}/${dep_name}" ]]; then
        rm "${DEPS_DIR}/${dep_name}"
        log_success "Removed symlink: ${DEPS_DIR}/${dep_name}"
    elif [[ -d "${DEPS_DIR}/${dep_name}" ]]; then
        log_warn "Not a symlink: ${DEPS_DIR}/${dep_name} (manual removal required)"
    fi

    # Remove from .local.cmake
    if [[ -f "$LOCAL_FILE" ]]; then
        sed -i.bak "/DEPS_${name_upper}_PATH/d" "$LOCAL_FILE"
        rm -f "${LOCAL_FILE}.bak"
        log_success "Removed from .local.cmake: DEPS_${name_upper}_PATH"
    fi

    echo ""
    echo "Local override removed for '${dep_name}'"
    echo "  - Will use remote source on next cmake configure"
}

cmd_sync() {
    log_info "Syncing all dependencies to local..."

    local local_root="${DEPS_LOCAL_ROOT:-$HOME/repos}"

    while IFS='|' read -r name git_url tag; do
        local target_dir="${local_root}/${name}"

        if [[ -d "$target_dir" ]]; then
            log_info "Updating ${name}..."
            (cd "$target_dir" && git fetch --all && git pull --ff-only 2>/dev/null) || \
                log_warn "Could not update ${name} (may have local changes)"
        else
            if [[ -n "$git_url" ]]; then
                log_info "Cloning ${name}..."
                git clone "$git_url" "$target_dir"
                if [[ "$tag" != "main" ]] && [[ "$tag" != "master" ]]; then
                    (cd "$target_dir" && git checkout "$tag")
                fi
            else
                log_warn "No git URL for ${name}, skipping"
            fi
        fi
    done < <(parse_deps_config "$CONFIG_FILE")

    log_success "Sync complete! Dependencies are in: $local_root"
}

cmd_bundle() {
    local output_dir="./dist"
    local format="tar.gz"
    local version=""

    # Parse bundle options
    while [[ $# -gt 0 ]]; do
        case $1 in
            -o|--output) output_dir="$2"; shift 2 ;;
            -f|--format) format="$2"; shift 2 ;;
            -v|--version) version="$2"; shift 2 ;;
            *) shift ;;
        esac
    done

    if [[ -z "$version" ]]; then
        version="$(date +%Y%m%d)"
    fi

    local bundle_name="deps-bundle-${version}"
    local work_dir=$(mktemp -d)
    local bundle_dir="${work_dir}/${bundle_name}"

    log_info "Creating dependency bundle: ${bundle_name}"

    mkdir -p "$bundle_dir"

    # Bundle each dependency
    while IFS='|' read -r name git_url tag; do
        log_info "Bundling ${name}..."

        local source_dir=""

        # Check for local source first
        if [[ -d "${DEPS_DIR}/${name}" ]]; then
            source_dir="${DEPS_DIR}/${name}"
        elif [[ -d "${PROJECT_ROOT}/vendor/${name}" ]]; then
            source_dir="${PROJECT_ROOT}/vendor/${name}"
        elif [[ -n "$git_url" ]]; then
            # Clone to temp
            local temp_clone="${work_dir}/clone_${name}"
            git clone --depth 1 --branch "$tag" "$git_url" "$temp_clone" 2>/dev/null || \
                git clone --depth 1 "$git_url" "$temp_clone"
            source_dir="$temp_clone"
        fi

        if [[ -n "$source_dir" ]] && [[ -d "$source_dir" ]]; then
            rsync -a --exclude='.git' "$source_dir/" "${bundle_dir}/${name}/"
            log_success "Bundled: ${name}"
        else
            log_warn "Could not bundle: ${name}"
        fi
    done < <(parse_deps_config "$CONFIG_FILE")

    # Create manifest
    cat > "${bundle_dir}/MANIFEST.txt" << EOF
# Dependency Bundle Manifest
# Generated: $(date -u +"%Y-%m-%d %H:%M:%S UTC")
# Version: ${version}

Dependencies:
$(parse_deps_config "$CONFIG_FILE" | while IFS='|' read -r n g t; do echo "  - $n ($t)"; done)
EOF

    # Create helper CMakeLists.txt
    cat > "${bundle_dir}/CMakeLists.txt" << 'EOF'
# Auto-generated CMakeLists.txt for bundled dependencies
# Add this directory as a subdirectory, or copy individual deps to your vendor/

cmake_minimum_required(VERSION 3.16)

file(GLOB dep_dirs "${CMAKE_CURRENT_SOURCE_DIR}/*")
foreach(dep_dir ${dep_dirs})
    if(IS_DIRECTORY ${dep_dir} AND EXISTS "${dep_dir}/CMakeLists.txt")
        get_filename_component(dep_name ${dep_dir} NAME)
        message(STATUS "Adding bundled dependency: ${dep_name}")
        add_subdirectory(${dep_dir})
    endif()
endforeach()
EOF

    # Create archive
    mkdir -p "$output_dir"
    local archive_path="${output_dir}/${bundle_name}.${format}"

    (cd "$work_dir" && tar -czf "$archive_path" "$bundle_name")

    # Cleanup
    rm -rf "$work_dir"

    log_success "Bundle created: ${archive_path}"
    echo "  Size: $(du -h "$archive_path" | cut -f1)"
}

# ============================================================================
# Main
# ============================================================================

# Parse global options
while [[ $# -gt 0 ]]; do
    case $1 in
        -c|--config) CONFIG_FILE="$2"; shift 2 ;;
        -d|--deps-dir) DEPS_DIR="$2"; shift 2 ;;
        -q|--quiet) QUIET=true; shift ;;
        -h|--help) usage ;;
        -*) log_error "Unknown option: $1"; exit 1 ;;
        *) break ;;
    esac
done

# Get command
COMMAND="${1:-help}"
shift || true

case "$COMMAND" in
    init)     cmd_init "$@" ;;
    status)   cmd_status "$@" ;;
    local)    cmd_local "$@" ;;
    unlocal)  cmd_unlocal "$@" ;;
    sync)     cmd_sync "$@" ;;
    bundle)   cmd_bundle "$@" ;;
    help)     usage ;;
    *)        log_error "Unknown command: $COMMAND"; usage ;;
esac
