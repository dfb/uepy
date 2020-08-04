# uepy
Implement UE4 game logic in Python

# misc notes
- for each class we want to be able to extend in python, we create a C++ bridge class that ferries calls back and forth to python; this class is never instantiated on its own but always as the engine object for a corresponding python instance
