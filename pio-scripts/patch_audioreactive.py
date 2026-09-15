Import("env")
import os
import json

def patch_audioreactive_lib(source, target, env):
    """Patch the AudioReactive library.json to exclude .cpp from compilation"""
    libdeps_dir = env.subst("$PROJECT_LIBDEPS_DIR/$PIOENV")
    library_json_path = os.path.join(libdeps_dir, "wled-audioreactive", "library.json")
    
    if os.path.exists(library_json_path):
        try:
            with open(library_json_path, 'r') as f:
                content = f.read()
                # Handle malformed JSON (missing opening brace)
                if not content.strip().startswith('{'):
                    content = '{' + content
                if not content.strip().endswith('}'):
                    content = content + '}'
                library_config = json.loads(content)
            
            # Add srcFilter to exclude all source files from library compilation
            if "build" not in library_config:
                library_config["build"] = {}
            
            if "srcFilter" not in library_config["build"]:
                library_config["build"]["srcFilter"] = ["-<*>"]
                
                with open(library_json_path, 'w') as f:
                    json.dump(library_config, f, indent=2)
                
                print("✓ Patched wled-audioreactive library.json to exclude .cpp from compilation")
        except Exception as e:
            print(f"Warning: Could not patch wled-audioreactive library.json: {e}")

# Run immediately when script is loaded
patch_audioreactive_lib(None, None, env)

