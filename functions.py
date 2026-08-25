# functions.py - bluestar.one
# github.com/bluestarone

# this file contains all the functions used by main_gui.py

# imports
import json


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
