# main_gui.py - bluestar.one
# github.com/bluestarone

# this file contains the main gui code


# imports
import customtkinter as ctk
import tkinter as tk
import functions as fct
import os
import shutil


# functions that cant be written in functions.py

def clear_gui():
    for widget in root.winfo_children():
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
    # Get the directory containing the exe
    game_directory = os.path.dirname(filepath)
    
    # Define what you want to copy
    # Option 1: Copy individual files
    files_to_copy = ["version.dll", "bbr2_mp.ini"]  # examples
    for file in files_to_copy:
        src = os.path.join(os.getcwd(), file)  # from launcher's current directory
        dst = os.path.join(game_directory, file)
        if os.path.isfile(src):
            shutil.copy2(src, dst)  # copy2 preserves metadata


def startup_page_2():
    global textbox
    clear_gui()
    filepath = find_file('Game_x64.exe')

    label = ctk.CTkLabel(root, text="If this exe was placed in the same directory as BBR2: IA's .exe file, the latter might have been discovered already and you dont need to take further actions. If this is not the case, please select BBR2: IA's .exe file now (usually C:/Program Files (x86)/Steam/steamapps/BBR2IA/Game_x64.exe)", text_color="white", font=("Arial", 16), wraplength=600)
    label.pack(pady=50)
    
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
            command=startup_page_3,
            height=30,
            fg_color="#0073ff",
            hover_color="#2600ff",
            text_color="white"
        )
    button2.pack(side="bottom", padx=5)

def startup_page_3():
    global textbox
    filepath = textbox.get("0.0", "end").strip()
    fct.update_json("exeFilepath", filepath)
    clear_gui()
    copy_files_to_game_directory(filepath)

    label = ctk.CTkLabel(root, text="files copied to the directory", text_color="white", font=("Arial", 16), wraplength=600)
    label.pack(pady=50)



# code
root = ctk.CTk()
root.title("Kobalt")
root.geometry("700x400")


if fct.read_json('firstLaunch') == 'True':

    label = ctk.CTkLabel(root, text="Welcome to Kobalt, first custom launcher for BBR2: Island Adventure.\n\nKobalt will bring you the joys of multiplayer directly within your game.\nPlease follow the upcoming instructions!\n\nKobalt was created by Nauzea (core multiplayer code) and BlueStar1 (app)", text_color="white", font=("Arial", 16))
    label.pack(pady=50)
    button = ctk.CTkButton(
                root,
                text='Next', 
                command=startup_page_2,
                fg_color="#0073ff",
                hover_color="#2600ff",
                text_color="white"
            )
    button.pack(side="right", padx=10, pady=10)



else :
    # Create a custom menu bar frame
    menubar = ctk.CTkFrame(root, fg_color="#2b2b2b", height=40)
    menubar.pack(side="top", fill="x")
    menubar.pack_propagate(False)

    # Function to create dropdown menus
    def create_dropdown(menu_name, options):
        def show_dropdown():
            dropdown_frame = ctk.CTkFrame(root, fg_color="#3b3b3b")
            dropdown_frame.place(x=button.winfo_x(), y=menubar.winfo_height())
            
            for option_label, option_command in options:
                btn = ctk.CTkButton(
                    dropdown_frame, 
                    text=option_label, 
                    command=option_command,
                    fg_color="#3b3b3b",
                    hover_color="#1f6aa0",
                    anchor="w",
                    width=150
                )
                btn.pack(fill="x", padx=5, pady=5)
            
            # Close dropdown when clicking outside
            def close_dropdown(e):
                if e.widget not in [button, dropdown_frame] and dropdown_frame.winfo_exists():
                    dropdown_frame.destroy()
            
            root.bind("<Button-1>", close_dropdown, add="+")
        
        button = ctk.CTkButton(
            menubar, 
            text=menu_name, 
            command=show_dropdown,
            fg_color="#2b2b2b",
            hover_color="#3b3b3b",
            text_color="white"
        )
        button.pack(side="left", padx=10, pady=8)

    # Add menus
    create_dropdown("File", [
        ("New", lambda: print("New")),
        ("Open", lambda: print("Open")),
        ("Exit", root.quit)
    ])

    create_dropdown("Edit", [
        ("Undo", lambda: print("Undo")),
        ("Redo", lambda: print("Redo"))
    ])

    create_dropdown("Help", [
        ("About", lambda: print("About"))
    ])

    # Main content area
    label = ctk.CTkLabel(root, text="Main Content", text_color="white", font=("Arial", 16))
    label.pack(pady=50)

root.mainloop()
