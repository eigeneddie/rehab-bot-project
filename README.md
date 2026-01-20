![Rehab_bot](https://github.com/eigeneddie/rehab-bot-project/blob/main/maxresdefault.jpg)

# REHAB BOT PROJECT
Lower-extremity rehabilitation project for post-stroke patient treatment using admittance control technique. 

This repo is for storing the code required to power Rehab-Bot.

The main scripts used is:
1. `main_new.py` located in `python_scripts/` which is the high-level controller module operated on an ARM Cortex-A72 (just a fancy way to say Raspberry pi) used to accept user input from the LCD user display located in `arduino_scripts/rehabProjectDisplayScript/rehabProjectDisplayScript.ino`
2. `active_modes.ino` located in `arduino_scripts/active_modes/`. This script is used to power the _active training mode_ feature which allows the robot to generate a virtual resistance for progressive resistive training.
3. `passive_modes.ino` located in `arduino_scripts/passive_modes/`. This script is used to activate the passive training mode functionality which is similar to how commercial continuous passive motion devices work.

# PUBLICATION AND VIDEO EXPLAINER
Rehab-bot is now an Open-Access paper [published](https://www.sciencedirect.com/science/article/pii/S2667241325000047) in the journal of [Cognitive Robotics](https://www.sciencedirect.com/journal/cognitive-robotics). I also made an [explainer YouTube video](https://www.youtube.com/watch?v=76rdyxkJO3A).
