# functions.py - bluestar.one
# github.com/bluestarone

# this file contains all the functions used by main_gui.py

# imports
import json
import requests


# functions

def log(text, filename="log.txt"):
    try:
        with open(filename, "a", encoding="utf-8") as file:
            file.write(text)
            file.write('\n')
    except:
       print("We fucked up, there's nothing we can do")

def update_json(key, value, filename="settings.json"):
    try:
        with open(filename, "r", encoding="utf-8") as file:
            var = json.load(file)
        var[key] = value
        with open(filename, "w", encoding="utf-8") as file:
            json.dump(var, file, indent=2)

    except FileNotFoundError:
        log(f'{filename} not found')
    except json.JSONDecodeError:
        log("Invalid JSON format")
    except Exception as e:
        log(f"Unexpected error: {e}")


def read_json(key, filename="settings.json"):
    try:
        with open(filename,"r", encoding="utf-8") as file:
            var = json.load(file)
        return var[key]

    except FileNotFoundError:
        log(f'{filename} not found')
    except json.JSONDecodeError:
        log("Invalid JSON format")
    except Exception as e:
        log(f"Unexpected error: {e}")



def validate_username(username, *args):
        """Limit to 15 chars and allow only alphanumeric"""
        content = username.get("0.0", "end-1c")  # Get all text
        
        # Remove non-alphanumeric characters
        filtered = ''.join(c for c in content if c.isalnum())
        
        # Limit to 15 characters
        if len(filtered) > 15:
            filtered = filtered[:15]
        
        # Update if changed
        if filtered != content:
            username.delete("0.0", "end")
            username.insert("0.0", filtered)