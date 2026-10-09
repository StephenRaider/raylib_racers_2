#pragma once
#include <string>
#include <vector>

#include "raylib.h"

// The grid's paint schemes: one entry per car slot (team livery with that car's
// number), read from assets/cars/f1_gearari/teams.json, and which slot each car in
// the race wears. teamColor() and the car model both go through this.
struct CarLivery {
    std::string team;   // e.g. "Papaya Masterkard F1"
    std::string key;    // base livery name, e.g. "papaya"
    std::string file;   // full path of the numbered livery PNG
    int number = 0;
    Color color{200, 200, 200, 255};  // for the HUD: the team's most recognisable colour
    std::string sheet;   // General Championship: the team's painted livery sheet (2048 x 2048 PNG), "" = none
    int model = 0;   // RR2: 0 the Mercedes in the team's own colour, 1..11 that stock 2013 car in its livery
};

// Reads teams.json, or without it one slot per liveries/*.png. Empty if neither exists.
std::vector<CarLivery> loadLiveries(const std::string& assetsDir);

void setLiveryTable(const std::vector<CarLivery>& table);
const std::vector<CarLivery>& liveryTable();
// Which slot each car (by race index) wears; cars beyond the list wear slot index % count.
void setCarLiveries(const std::vector<int>& slotOfCar);
int carLivery(int carIndex);

// Raylib Racers 2: teams are Team 1, Team 2 ... and any of them can be any colour.
// The ten preset colours (also the ten solid liveries of the 2013 Mercedes), in picker order.
int presetCount();
Color presetColor(int i);
const char* presetName(int i);
// Renames the table's teams (in order of first appearance) and gives each a preset colour.
void neutralTeams(std::vector<CarLivery>& table);
// A livery slot's team colour: the HUD, minimap and the car's paint.
void setSlotColor(int slot, Color c);

// Raylib Racers 2's stock cars: assets/cars/f1_2013_01 .. 11, each in its own livery (car.json has the
// livery's main colour). A team drives one of them, or the Mercedes (model 2) in any colour (model 0).
constexpr int kStockCars = 11;
void loadStockCars(const std::string& assetsDir);
bool stockPresent(int model);   // 1..kStockCars: the car is in assets/cars
Color stockColour(int model);   // the livery's main colour
void setSlotCar(int slot, int model, Color colour);   // the team colour of the HUD goes with it
int slotModel(int slot);
