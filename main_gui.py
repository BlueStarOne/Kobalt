# main_gui.py - bluestar.one
# github.com/bluestarone

# this file contains the main gui code


# imports

import customtkinter as ctk
import tkinter as tk
import functions as fct
import os
import shutil
import random
import requests
import webbrowser
import CTkMenuBar
from CTkMessagebox import CTkMessagebox
from PIL import Image

# variables

check_for_updates = "Check for updates"

# code

def kobalt_version_check(is_triggered_by_user = False):
    url = "https://raw.githubusercontent.com/BlueStarOne/Kobalt/refs/heads/main/settings.json"
    try:
        response = requests.get(url, timeout=2)
        data = response.json()
        latest_version = data["kobaltVersion"]
        
        if latest_version > str(fct.read_json("kobaltVersion")):
            clear_gui()
            
            update = ctk.CTkLabel(root, text="New update available!", text_color="white", font=("Arial", 32), wraplength=600)
            update.pack(pady=50)

            label = ctk.CTkLabel(root, text=f"New version available: v{latest_version}\n\nChangelog: {data['kobaltChangelog']}", text_color="white", font=("Arial", 16), wraplength=600)
            label.pack(pady=10)

            button = ctk.CTkButton(
                root,
                text='Download', 
                command=lambda: webbrowser.open_new("https://github.com/BlueStarOne/Kobalt/releases"),
                fg_color="#0073ff",
                hover_color="#2600ff",
                text_color="white"
            )
            button.pack(side="bottom", padx=10, pady=50)

            return True
        
        if is_triggered_by_user:
            message = CTkMessagebox(title="No new update found", message="You are running the latest Kobalt version", icon="info", option_1="OK")
            if message.get() == "OK":
                root.detroy
            return
        return False
    except:
        return None  # Network error


# first startup

def clear_gui():
    for widget in root.winfo_children():
        if widget != menu:  # Skip the menu
            widget.destroy()

def select_file():
    global textbox
    filetypes = (
        ('exe', '*.exe'),
        ('All files', '*.*')
    )

    filename = tk.filedialog.askopenfilename(
        title='Open a file',
        initialdir='/',
        filetypes=filetypes)

    textbox.delete("0.0", "end")
    textbox.insert("0.0", filename)

def find_file(filename):
    if os.path.isfile(filename):
        return os.path.abspath(filename)
    else:
        return None

def copy_files_to_game_directory(filepath):
    """Copy launcher files to the game directory."""
    game_directory = os.path.dirname(filepath)
    files_to_copy = ["version.dll", "bbr2_mp.ini"]
    
    for file in files_to_copy:
        src = os.path.join(os.getcwd(), file)
        dst = os.path.join(game_directory, file)
        
        if os.path.isfile(src):
            try:
                shutil.copy2(src, dst)
            except PermissionError:
                return f"ERROR: Cannot copy {file}. It may be in use.\nPlease close the game and any running instances of Kobalt, then try again."  # Return False to indicate failure
    
    return True  # Return True if successful


def startup_page_1():
    clear_gui()
    label = ctk.CTkLabel(root, text="Welcome to Kobalt, first custom launcher for BBR2: Island Adventure.\n\nKobalt will bring you the joys of multiplayer directly within your game.\n\nTo use Kobalt, you need to own a legal copy of Beach Buggy Racing 2: Island Adventure\n\nPlease follow the upcoming instructions!\n\nKobalt was created by Nauzea (core multiplayer code) and BlueStar1 (app)", text_color="white", font=("Arial", 16))
    label.pack(pady=50)
    button = ctk.CTkButton(
                root,
                text='Next', 
                command=startup_page_2,
                fg_color="#0073ff",
                hover_color="#2600ff",
                text_color="white"
            )
    button.pack(side="bottom", padx=10, pady=10)

def startup_page_2():
    clear_gui()
    important = ctk.CTkLabel(root, text="Acknowledgment", text_color="white", font=("Arial", 32), wraplength=600)
    important.pack(pady=50)

    tos = ctk.CTkTextbox(root, width=650, height=300)
    tos.insert("0.0", """KOBALT - USER AGREEMENT & DISCLAIMER\n\nIMPORTANT: Please read carefully before using Kobalt.\n\n1. TERMS OF SERVICE VIOLATION WARNING\n\nBy using Kobalt, you acknowledge that:\n\n- Vector Unit's Terms of Service prohibit code injection and third-party software that modifies or interferes with their services, regardless of the technical method used.\n- Using Kobalt likely violates Vector Unit's Code of Conduct, specifically the clauses forbidding:\n\t- Unauthorized third-party software designed to modify or interfere with the Services\n\t- Modification of game functionality without express written consent\n\t- Any activity deemed outside the spirit or intent of the Services\n- Your account may be terminated. Vector Unit explicitly reserves the right to suspend or terminate your access to any of their games at any time, for any reason, without notice.\n\n2. RISK ACKNOWLEDGMENT\n\nI understand and accept that:\n\n- My account in Beach Buggy Racing 2: Island Adventure or any Vector Unit game may be permanently suspended or terminated as a result of using Kobalt\n- I am using Kobalt entirely at my own risk\n- The developers of Kobalt are not responsible for any account termination, data loss, or other consequences\n- I have no recourse against Vector Unit or Kobalt developers if my account is terminated\n\nKOBALT - CODE OF CONDUCT\n\nYour use of Kobalt is governed by the following Code of Conduct (the "Code"). It is your responsibility to know, understand, and abide by this Code. Violation of these rules may result in disciplinary action, including but not limited to:\n\n- Username change or reset\n- Chat suspension or mute\n- Temporary ban from Kobalt\n- Permanent ban and account termination\n\n1. FAIR PLAY & NO CHEATING\n\nYou agree that you will not:\n\n- Use cheats, exploits, hacks, mods, or automation tools within Kobalt (beyond the official Kobalt launcher itself)\n- Attempt to gain unauthorized access to other players' accounts or sessions\n- Exploit game glitches, bugs, or unintended mechanics for competitive advantage\n- Modify game files or use software to interfere with gameplay\n- Use aimbots, wallhacks, speed hacks, or any form of assistance that provides unfair advantage\n- Engage in any other form of cheating or unsportsmanlike conduct\n\nViolations will result in immediate ban from Kobalt.\n\n2. APPROPRIATE USERNAMES\n\nYour username must be respectful and appropriate. You agree that you will not:\n\n- Use usernames containing offensive, vulgar, hateful, or discriminatory language\n- Use racist, sexist, homophobic, transphobic, or ableist slurs or references\n- Impersonate other players, developers, or public figures\n- Use usernames that promote illegal activity or harm\n- Attempt to circumvent username filters using misspellings or alternative spellings\n\nInappropriate usernames will be automatically or manually changed without notice.\n\n3. RESPECTFUL COMMUNICATION\n\nIf Kobalt includes chat or messaging features, you agree that you will not:\n\n- Post or transmit content that is unlawful, harmful, threatening, abusive, harassing, or defamatory\n- Use profanity, obscene language, or sexually explicit content\n- Engage in hate speech, including racism, sexism, homophobia, transphobia, religious discrimination, or ableism\n- Harass, bully, or target other players\n- Spam, flood, or post repetitive messages\n- Share other players' personal information without consent (addresses, phone numbers, emails, etc.)\n- Advertise external services, games, or websites without permission\n- Engage in sexual or romantic solicitation\n\nViolations may result in chat suspension, mute, or account ban.\n\n4. RESPECTFUL GAMEPLAY & COMMUNITY\n\nYou agree that you will not:\n\n- Intentionally disrupt or interfere with other players' gameplay\n- Engage in toxic behavior, rage quitting, or intentional losing/griefing\n- Make false reports or abuse the report system\n- Encourage or assist other players in violating this Code of Conduct\n- Create multiple accounts to evade bans or restrictions\n\n5. ENFORCEMENT & APPEALS\n\nKobalt developers reserve the right to determine what conduct violates this Code of Conduct. Disciplinary action may include warnings, suspensions, or permanent bans. Bans are typically permanent and non-negotiable. Attempting to evade bans through alternate accounts will result in those accounts being banned as well.\n\n6. ACKNOWLEDGMENT\n\nBy playing on Kobalt, you agree to follow this Code of Conduct. Violations may result in loss of access to Kobalt services.\n"""
)
    tos.configure(state="disabled")
    tos.pack(pady=10)

    frame = ctk.CTkFrame(root, fg_color='transparent')
    frame.pack()

    def on_agree_toggle():
        if agree_checkbox.get():
            disagree_checkbox.deselect()
            button.grid(row=0, column=2, padx=50, pady=5)  # Show button

    def on_disagree_toggle():
        if disagree_checkbox.get():
            agree_checkbox.deselect()
            button.grid_remove()  # Hide button

    # Create checkboxes with commands
    agree_checkbox = ctk.CTkCheckBox(frame, text="I agree", text_color="white", command=on_agree_toggle)
    agree_checkbox.grid(row=0, column=1, padx=10, pady=5)

    disagree_checkbox = ctk.CTkCheckBox(frame, text="I don't agree", text_color="white", command=on_disagree_toggle)
    disagree_checkbox.grid(row=0, column=0, padx=10, pady=5)
    disagree_checkbox.select()

    button = ctk.CTkButton(
        frame,
        text='Next', 
        command=startup_page_3,
        height=30,
        fg_color="#0073ff",
        hover_color="#2600ff",
        text_color="white"
    )
    button.grid(row=0, column=2, padx=50, pady=5)
    button.grid_remove()  # Hide button initially since disagree is selected by default




    
def startup_page_3():
    global textbox
    clear_gui()
    filepath = find_file('Game_x64.exe')

    step1 = ctk.CTkLabel(root, text="Step 1", text_color="white", font=("Arial", 32), wraplength=600)
    step1.pack(pady=50)

    label = ctk.CTkLabel(root, text="If this exe was placed in the same directory as BBR2: IA's .exe file, the latter might have been discovered already and you dont need to take further actions. If this is not the case, please select BBR2: IA's .exe file now (usually C:/Program Files (x86)/Steam/steamapps/BBR2IA/Game_x64.exe)", text_color="white", font=("Arial", 16), wraplength=600)
    label.pack(pady=10)
    
    # Create a frame to hold the textbox and button
    button_frame = ctk.CTkFrame(root, fg_color='transparent')
    button_frame.pack(pady=10)  # Center it horizontally by default
    
    textbox = ctk.CTkTextbox(button_frame, height=30, width=450)
    if filepath:
        textbox.insert("0.0", filepath)
        textbox.pack(side="left", padx=5)

    else :
        textbox.insert("0.0", 'e.g. C:/Program Files (x86)/Steam/steamapps/BBR2IA/Game_x64.exe')
        textbox.pack(side="left", padx=5)
    
    button = ctk.CTkButton(
        button_frame,
        text='Browse', 
        command=select_file,
        height=30,
        fg_color="#0073ff",
        hover_color="#2600ff",
        text_color="white"
    )
    button.pack(side="left", padx=5)

    button2 = ctk.CTkButton(
            root,
            text='Next', 
            command=startup_page_4,
            height=30,
            fg_color="#0073ff",
            hover_color="#2600ff",
            text_color="white"
        )
    button2.pack(side="bottom", padx=10, pady=10)

def startup_page_4():
    global textbox
    filepath = textbox.get("0.0", "end").strip()
    fct.update_json("exeFilepath", filepath)
    clear_gui()

    step1 = ctk.CTkLabel(root, text="Step 1", text_color="white", font=("Arial", 32), wraplength=600)
    step1.pack(pady=50)

    attempt = ctk.CTkLabel(root, text="Copying files...", text_color="white", font=("Arial", 16), wraplength=600)
    attempt.pack(pady=10)

    copied_files = copy_files_to_game_directory(filepath)

    if copied_files == True:
        success = ctk.CTkLabel(root, text="Files copied!", text_color="white", font=("Arial", 16), wraplength=600)
        success.pack(pady=10)
        root.after(2000, startup_page_5)

    else:
        label = ctk.CTkLabel(root, text=copied_files, text_color="white", font=("Arial", 16), wraplength=600)
        label.pack(pady=10)

def startup_page_5():
    clear_gui()

    step1 = ctk.CTkLabel(root, text="Step 2", text_color="white", font=("Arial", 32), wraplength=600)
    step1.pack(pady=50)

    label = ctk.CTkLabel(root, text="What would you like your username to be?\nDon't worry, you can still change that later", text_color="white", font=("Arial", 16), wraplength=600)
    label.pack(pady=10)

    global username
    username = ctk.CTkTextbox(root, height=30, width=150, activate_scrollbars=False)
    username.insert("0.0", f"Player{random.randint(0000, 9999)}")
    username.pack(pady=10)

    # Bind to key release event using lambda
    username.bind("<KeyRelease>", lambda event: fct.validate_username(username))

    button = ctk.CTkButton(
            root,
            text='Next', 
            command=startup_page_6,
            height=30,
            fg_color="#0073ff",
            hover_color="#2600ff",
            text_color="white"
        )
    button.pack(side="bottom", padx=10, pady=10)

def startup_page_6():
    global username
    tempusername = username.get("0.0", "end").strip()
    fct.update_json("defaultUsername", tempusername)
    clear_gui()
    label = ctk.CTkLabel(root, text=f"You're all set! Welcome to the race, {tempusername}! We hope you'll have a wonderful racing here!", text_color="white", font=("Arial", 16), wraplength=600)
    label.pack(pady=50)
    fct.update_json("firstLaunch", "False")
    root.after(5000, main_page)


# mainpage

def main_page():
    clear_gui()
    img = ctk.CTkImage(light_image=Image.open("kobalt.png"), dark_image=Image.open("kobalt.png"), size=(566, 148))

    label = ctk.CTkLabel(root,text=None, image=img, fg_color='transparent')
    label.pack(pady=50)

    frame = ctk.CTkFrame(root, fg_color='transparent')
    frame.pack(pady=10)

    button = ctk.CTkButton(
        frame,
        text='Join a server', 
        command=join_servers_page,
        height=30,
        fg_color="#0073ff",
        hover_color="#2600ff",
        text_color="white"
    )
    button.pack(side="left", padx=5)

    button2 = ctk.CTkButton(
        frame,
        text='Create a server', 
        command=lambda: kobalt_version_check(True),
        height=30,
        fg_color="#0073ff",
        hover_color="#2600ff",
        text_color="white"
    )
    button2.pack(side="right", padx=5)
    


# Join a server

def join_servers_page():
    clear_gui()
    img = ctk.CTkImage(light_image=Image.open("kobalt.png"), dark_image=Image.open("kobalt.png"), size=(566, 148))
    
    label = ctk.CTkLabel(root,text=None, image=img, fg_color='transparent')
    label.pack(pady=50)
    serverlist = ctk.CTkScrollableFrame(root, width=200, height=200)
    serverlist.pack()





global root
root = ctk.CTk()
root.title("Kobalt")
root.geometry("700x500")

# menu

menu = CTkMenuBar.CTkTitleMenu(master=root, x_offset=100, y_offset=7)

edit_button = menu.add_cascade("Edit")
edit_dropdown = CTkMenuBar.CustomDropdownMenu(widget=edit_button)
edit_dropdown.add_option(option="Preferences", command="")

about_button = menu.add_cascade("Help")
about_dropdown = CTkMenuBar.CustomDropdownMenu(widget=about_button, separator_color="white")
about_dropdown.add_option(option="Restart tutorial", command=startup_page_1)
about_dropdown.add_option(option="Check for updates", command=lambda: kobalt_version_check(True))
about_dropdown.add_option(option="About", command="")


if not kobalt_version_check():

    if fct.read_json('firstLaunch') == 'True':

        startup_page_1()

    else :

        main_page()

root.mainloop()
