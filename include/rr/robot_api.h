/*
 * Raylib Racers - robot (driver) plugin API
 *
 * A robot is a shared library (.so / .dll / .dylib) that exports one C function:
 *
 *     RR_EXPORT const RRRobotApi* rr_robot_entry(void);
 *
 * The host calls create() once per car driven by the robot, then drive() at a
 * fixed rate (50 Hz by default) and destroy() at the end of the race. The same
 * library can drive several cars at once; keep all state in the instance
 * pointer returned by create(), not in globals.
 *
 * The sensor model follows TORCS / the Simulated Car Racing (SCR) championship:
 * angle to the track axis, normalised lateral position, 19 track-edge
 * range finders and 36 opponent sectors. On top of that the robot gets the full
 * track geometry at create() time and its world pose, like a TORCS robot does,
 * so it can plan racing lines.
 *
 * Plain C99 so robots can be written in C, C++ or anything with a C FFI.
 *
 * Conventions: SI units (m, s, rad, kg). World frame is an x/y plane seen from
 * above (heights come separately, see RRTrackPoint3), yaw is counter-clockwise
 * from +x. Car body frame: x forward, y to the left. Positive steering turns left.
 */
#ifndef RR_ROBOT_API_H
#define RR_ROBOT_API_H

#ifdef __cplusplus
extern "C" {
#endif

#define RR_ABI_VERSION 14
/* Robots built for ABI 2 to 13 still load: later versions only appended
 * fields to RRTrackInfo, RRCarSpec, RRRobotConfig, RRSensors and RRControl. */
#define RR_ABI_MIN_VERSION 2

#define RR_NUM_TRACK_SENSORS 19
#define RR_NUM_OPPONENT_SENSORS 36
#define RR_SENSOR_RANGE 200.0f
#define RR_MAX_GEARS 8
#define RR_NUM_DEBUG 8
#define RR_MAX_NEARBY 8
#define RR_MAX_CARS 32

/* Tyre compounds: softer is grippier but wears faster and works cooler.
 *                    soft      medium    hard
 * New-tyre grip:     x1.05     x1.0      x0.965
 * Wear rate:         x2.0      x1.0      x0.72
 * Working window:    85-105 C  95-115 C  105-125 C
 * Worn grip: 1 - 0.07 * wear, falling off a cliff past wear 0.7 (-0.8 per unit beyond).
 * Temperature: -0.25% grip per C below the window, -0.2% per C above (at most -20%);
 * wear x(1 + 0.06 per C above the window), x(1 + 0.015 per C below it).
 * Tyres come off the warmers at 80 C (race start and after a stop). */
/* Sessions (ABI 8). A race weekend runs practice, then qualifying, then the
 * race; each car practises and qualifies alone. */
#define RR_SESSION_RACE 0
#define RR_SESSION_PRACTICE 1    /* alone on track, up to session_laps laps, any tyres, pits open */
#define RR_SESSION_QUALIFYING 2  /* alone on track: an out lap and two flying laps */
#define RR_SESSION_TEST 3        /* testing mode: alone, recorded, pits closed */
/* Bytes of weekend memory the host gives each car (RRRobotConfig.memory). */
#define RR_SESSION_MEMORY (256 * 1024)  /* keep in step with rr::RR_SESSION_MEMORY_BYTES */

#define RR_TIRE_SOFT 1
#define RR_TIRE_MEDIUM 2
#define RR_TIRE_HARD 3

/* Pit service time: RR_PIT_SERVICE_BASE + max(fuel / RR_PIT_FUEL_RATE, RR_PIT_TIRE_CHANGE if
 * changing tyres) + RR_PIT_REPAIR_PER_1000 per 1000 damage if repairing, all
 * times the car's pit_service_scale. */
#define RR_PIT_SERVICE_BASE 2.0f      /* s: jacked up and dropped */
#define RR_PIT_FUEL_RATE 2.5f         /* l/s */
#define RR_PIT_TIRE_CHANGE 3.5f       /* s, alongside refuelling */
#define RR_PIT_REPAIR_PER_1000 1.0f   /* s per 1000 damage */
/* Two-compound rule (RRSensors.two_compound_rule): a car that finishes the
 * race without having used two different compounds gets this time penalty. */
#define RR_TWO_COMPOUND_PENALTY 30.0f

/* RRSensors.pit_state */
#define RR_PIT_NONE 0        /* racing */
#define RR_PIT_LANE 1        /* in the pit lane (speed limited) */
#define RR_PIT_SERVICE 2     /* stopped in the box, crew working: controls are ignored */
#define RR_PIT_DONE 3        /* service finished, still in the pit lane */

#if defined(_WIN32)
#define RR_EXPORT __declspec(dllexport)
#else
#define RR_EXPORT __attribute__((visibility("default")))
#endif

/* One sample of the track centreline. Samples are spaced roughly 1 m apart and
 * the track is closed: the sample after the last one is sample 0. */
typedef struct RRTrackPoint {
    float x, y;          /* centreline position */
    float dir_x, dir_y;  /* unit tangent in race direction */
    float s;             /* distance from the start line along the centreline */
    float half_width;    /* half the tarmac width; edges at +/- half_width along the left normal (-dir_y, dir_x) */
    float curvature;     /* signed, 1/m, positive = turning left */
} RRTrackPoint;

/* The track's third dimension (ABI 9), one per RRTrackPoint (same index).
 * Distances (s, curvature) stay measured in plan view. On a flat track every
 * field is 0. What the car feels:
 *   - grade: gravity pulls it back by g * grade uphill (pushes it on downhill);
 *   - bank: a banked road holds it in a turn towards the low side, and pushes the
 *     tyres into the road (more grip) in that turn;
 *   - vert_curvature: v^2 * vert_curvature adds to g in the tyre load, so a
 *     compression (+) gives more grip and a crest (-) less, none at all once
 *     v^2 * -vert_curvature reaches g. */
typedef struct RRTrackPoint3 {
    float z;               /* road height on the centreline, m */
    float bank;            /* rad, + = left edge higher; the road surface at lateral y is at z + y * tan(bank) */
    float grade;           /* dz/ds, + = uphill in the race direction */
    float vert_curvature;  /* d(grade)/ds, 1/m: + = compression (sag), - = crest */
} RRTrackPoint3;

/* The pit lane runs alongside the track on one side. Distances are along the
 * track centreline (s, may wrap past the start line); lateral offsets are
 * signed like track_pos (+ = left of the centreline), in metres.
 *
 *   entry_s ... lane_start_s : leave the track and move into the lane
 *   lane_start_s ... lane_end_s : the lane proper, behind a wall, speed limited
 *   lane_end_s ... exit_s : rejoin the track
 */
typedef struct RRPitInfo {
    int has_pit;
    int side;              /* +1 left of the track, -1 right */
    float entry_s, lane_start_s, lane_end_s, exit_s;
    float lane_offset;     /* lateral centre of the fast lane */
    float box_offset;      /* lateral centre of the pit boxes */
    float speed_limit;     /* m/s, enforced between lane_start_s and lane_end_s */
} RRPitInfo;

/* A corner of the track (ABI 8), numbered from the start line like the
 * turns of a real circuit, so a robot can keep notes per corner. The host
 * finds them from the centreline curvature; ids are the same in every
 * session of a weekend. */
typedef struct RRTurn {
    int id;              /* 1, 2, ... in race order */
    int direction;       /* +1 left, -1 right */
    float start_s, apex_s, end_s;  /* along the centreline, m (end_s may be less than start_s across the line) */
    float min_radius;    /* m, at the apex */
    float angle;         /* rad turned through, always positive */
} RRTurn;

/* A DRS zone (ABI 10): where the rear-wing flap may be opened. The detection
 * point is where the gap to the car ahead is timed; a car within
 * RR_DRS_GAP s of it may open the flap from start_s to end_s. All values are
 * track s in metres and may wrap across the start line. */
typedef struct RRDrsZone {
    float detect_s, start_s, end_s;
} RRDrsZone;

#define RR_DRS_GAP 1.0f          /* s: how close behind the car ahead at the detection point */
#define RR_DRS_FIRST_LAP 3       /* races: DRS is off for the first two laps */
/* RRSensors.drs_state */
#define RR_DRS_NONE 0            /* this car has no DRS, or it is not allowed (wet, first laps, pit lane) */
#define RR_DRS_OFF 1             /* allowed, but this car has not earned it for the zone ahead */
#define RR_DRS_ARMED 2           /* earned at the detection point: open the flap when the zone starts */
#define RR_DRS_AVAILABLE 3       /* in the zone and earned: set RRControl.drs to open the flap */
#define RR_DRS_OPEN 4            /* the flap is open */

typedef struct RRTrackInfo {
    const char* name;
    float length;        /* centreline length, m */
    float runoff;        /* distance from tarmac edge to the barrier, m */
    int num_points;
    const RRTrackPoint* points;
    RRPitInfo pit;

    /* --- ABI 8 --- */
    int num_turns;
    const RRTurn* turns;

    /* --- ABI 9 --- */
    const RRTrackPoint3* points3;  /* num_points entries, parallel to points */

    /* --- ABI 10 --- */
    int num_drs_zones;
    const RRDrsZone* drs_zones;
} RRTrackInfo;

typedef struct RRCarSpec {
    float mass;              /* kg, with driver */
    float length, width;     /* body size, m */
    float wheelbase;         /* m */
    float cg_to_front;       /* CG to front axle, m */
    float cg_to_rear;        /* CG to rear axle, m */
    float max_steer;         /* road-wheel angle at steer = 1, rad */
    float tire_mu;           /* peak tyre friction coefficient on tarmac */
    float drag_coeff;        /* 0.5 * rho * Cd * A, so drag force = drag_coeff * v^2 (N) */
    float downforce_coeff;   /* 0.5 * rho * Cl * A, so downforce = downforce_coeff * v^2 (N) */
    float max_rpm;
    float wheel_radius;      /* m */
    float final_drive;
    int num_gears;           /* forward gears */
    float gear_ratios[RR_MAX_GEARS]; /* [0] = 1st gear */
    float max_brake_force;   /* N, total over all wheels */
    float fuel_capacity;     /* litres */
    float fuel_density;      /* kg per litre: fuel adds fuel * fuel_density to the mass */

    /* --- ABI 3 --- */
    float cg_height;         /* m */
    float track_front, track_rear;  /* wheel-centre track widths, m */
    float downforce_front;   /* aero balance: share of the downforce on the front axle */
    float brake_front;       /* brake bias: share of the brake force on the front axle */
    float max_power;         /* W, peak engine power */
    float tire_wear_scale;   /* tyre wear multiplier from this car's development (1 = baseline) */
    float fuel_use_scale;    /* fuel use multiplier from this car's development (1 = baseline) */
    float pit_service_scale; /* pit crew time multiplier (1 = baseline, < 1 = faster crew) */

    /* --- ABI 10 --- 0 when the car has no KERS / DRS */
    float kers_power;        /* W, most the motor-generator deploys or recovers (spec: 120 kW) */
    float kers_energy;       /* J the store may release per lap (spec: 4 MJ) */
    float kers_harvest;      /* J the motor-generator may recover per lap (spec: 4 MJ) */
    float kers_store;        /* J, capacity of the store (spec: 4 MJ) */
    float drs_drag_scale;    /* drag coefficient with the flap open (1 = no DRS) */
    float drs_downforce_scale; /* downforce with the flap open; the loss is all at the rear axle */
} RRCarSpec;

/* A nearby car, for racecraft (overtaking, defending, pit timing). */
typedef struct RROpponent {
    int car_index;
    int race_pos;
    float ds;            /* track distance from us to them, + = ahead, wrapped to (-length/2, length/2] */
    float lateral;       /* their offset from the centreline, m, + = left */
    float speed;         /* their speed along their heading, m/s */
    float rel_x, rel_y;  /* their position in our body frame, m (x forward, y left) */
    float rel_yaw;       /* their heading minus ours, rad */
    int pit_state;
    int laps_ahead;      /* their completed laps minus ours */
} RROpponent;

/* One line of the timing screen: what a team sees about every car (no
 * positions on track, no tyre wear: those are the car's own secrets). */
typedef struct RRTimingEntry {
    int car_index;
    int race_pos;
    int laps_done;
    float gap_to_leader;  /* s at the same point of the track (-1 until timed) */
    float gap;            /* s from us: + = ahead of us in the race, - = behind (laps included) */
    float dist_raced;     /* m */
    float last_lap, best_lap;  /* s, 0 until set */
    int pit_state;        /* RR_PIT_* right now */
    int pit_stops;
    int tire_compound;    /* RR_TIRE_* fitted now */
    int laps_on_tires;
    int compounds_used;   /* bit (1 << RR_TIRE_*) for each compound used so far */
    int finished, dnf;
} RRTimingEntry;

/* Optional setup a robot can change inside create(). The host fills defaults
 * before calling create(). */
typedef struct RRRobotConfig {
    /* Directions of the 19 track range finders, degrees relative to the car
     * heading, positive to the left. Default: -90 -75 -60 -45 -30 -20 -15 -10 -5
     * 0 5 10 15 20 30 45 60 75 90 (the SCR default, mirrored to our left-positive
     * convention). */
    float track_sensor_angles[RR_NUM_TRACK_SENSORS];
    int auto_gear;       /* 1 (default): the host shifts gears; 0: robot sets RRControl.gear */
    float initial_fuel;  /* litres at the start (default: full tank) */
    int tire_compound;   /* starting tyres, RR_TIRE_* (default medium). When the team has chosen
                            the starting tyres (starting_compound_set below), the host
                            fills that choice in and ignores changes. */

    /* --- ABI 5 --- race information for planning at create() (read only) */
    int race_laps;
    int two_compound_rule;  /* 1: two different compounds must be used in the race */
    float fuel_rate;        /* race multiplier on fuel use (--fuel-rate, 1 = normal) */
    float wear_rate;        /* race multiplier on tyre wear (--wear-rate, 1 = normal) */
    float ambient_temp;     /* C */
    int starting_compound_set;  /* 1: the team chose the starting tyres (already in tire_compound) */

    /* --- ABI 6 --- */
    int pits_closed;        /* 1: no pit stops in this session (testing): plan to run to the flag */
    int starting_fuel_set;  /* 1: the team chose the starting fuel (already in initial_fuel); changes are ignored */

    /* --- ABI 8 --- */
    int session;            /* RR_SESSION_* this car is about to drive */
    int session_laps;       /* laps in this session (practice: the lap limit) */
    /* Weekend memory: RR_SESSION_MEMORY bytes the host keeps for this car from
     * practice through qualifying to the race, then wipes. Zeroed at the start
     * of the weekend. Write notes here (braking points, grip per turn, tyre
     * life); it is the only thing that carries between sessions, and robots
     * may not keep files of their own. */
    unsigned char* memory;
    int memory_size;
} RRRobotConfig;

typedef struct RRSensors {
    double time;          /* race time, s */
    float dt;             /* time since the previous drive() call, s */

    /* SCR-style sensors */
    float angle;          /* car heading minus track direction, rad, in (-pi, pi]; positive = pointing left of the track axis */
    float track_pos;      /* 0 on the centreline, +1 at the left tarmac edge, -1 at the right; |x| > 1 means off the tarmac */
    float track[RR_NUM_TRACK_SENSORS];        /* distance to the tarmac edge along each range finder, m (max RR_SENSOR_RANGE); -1 when off the tarmac */
    float opponents[RR_NUM_OPPONENT_SENSORS]; /* nearest opponent in each 10 degree sector, m (RR_SENSOR_RANGE if none). Sector i covers [-180 + 10i, -170 + 10i) degrees, 0 = straight ahead, positive = left */
    float speed_x;        /* longitudinal speed, m/s */
    float speed_y;        /* lateral speed, m/s, positive = sliding left */
    float yaw_rate;       /* rad/s */
    float rpm;
    int gear;             /* -1 reverse, 0 neutral, 1..num_gears */
    float wheel_spin;     /* how far the driven (rear) tyres are past their grip limit: 0 = gripping, 0.2 = asking 20% more than they can give */
    float damage;         /* accumulated collision damage, arbitrary units, repaired in the pits. Growing linearly to
                             8000 it costs up to 35% downforce, 12% engine power and 8% mechanical grip, and adds 10% drag */

    /* race state */
    float dist_from_start;  /* distance along the centreline since the start line on this lap, m */
    float dist_raced;       /* total distance covered since the start, m (negative on the grid behind the line) */
    int lap;                /* current lap, 1-based */
    int race_laps;          /* laps in the race */
    int race_pos;           /* 1 = leader */
    int num_cars;
    float cur_lap_time;
    float last_lap_time;    /* 0 until a lap is completed */
    float best_lap_time;    /* 0 until a lap is completed */

    /* ground truth (TORCS robots have this too) */
    float x, y, yaw;        /* world pose */
    int track_index;        /* nearest RRTrackPoint */
    int on_track;           /* 1 if on the tarmac */

    /* consumables */
    float fuel;             /* litres left; at 0 the engine stops */
    float tire_wear[2];     /* front, rear: 0 new .. 1 worn out */
    float tire_grip;        /* grip multiplier from compound and wear, both axles averaged (1 = new medium); axle_grip adds temperature */
    int tire_compound;      /* RR_TIRE_* */
    int laps_on_tires;

    /* pit */
    int pit_state;          /* RR_PIT_* */
    int pit_stops;          /* completed stops */
    float pit_box_s;        /* where this car's box is (track s) */
    float service_time_left;/* s, while RR_PIT_SERVICE */

    /* other cars, nearest first by track distance */
    int num_nearby;
    RROpponent nearby[RR_MAX_NEARBY];

    /* --- ABI 3 --- */
    /* What the tyres are doing: force asked of each axle over what it can give,
     * worst wheel of the axle. Around 0.9-1.0 is the limit; past ~1.1 the axle
     * is sliding (front: understeer; rear: oversteer or wheelspin). */
    float grip_use[2];      /* front, rear */
    float slip_angle[2];    /* front, rear, rad */
    float accel_x, accel_y; /* body-frame acceleration, m/s^2 (y + = left) */
    float wheel_load[4];    /* N: front left, front right, rear left, rear right */
    /* Blue flag: a car that is lapping us is close behind. Let it by; holding
     * it up for more than RR_BLUE_FLAG_LIMIT seconds costs a time penalty. */
    int blue_flag;          /* 1 while shown */
    int blue_flag_car;      /* index of the car to let by, -1 if none */
    float blue_flag_ds;     /* its track distance from us, m (negative = behind) */
    int penalties;          /* time penalties so far */
    float penalty_time;     /* s, added to the race time */

    /* --- ABI 4 --- */
    float tire_temp[2];        /* front, rear tyre temperature, C */
    float tire_temp_window[2]; /* the fitted compound's working window: low, high, C */
    float axle_grip[2];        /* front, rear grip multiplier now: compound x wear x temperature (tire_grip leaves out temperature) */
    float ambient_temp;        /* C */
    float slipstream;          /* drag reduction from a car ahead, 0..0.45 */
    float dirty_air;           /* downforce lost to a car ahead, 0..0.10 (the front loses more) */

    /* --- ABI 5 --- */
    /* The timing screen, in race order (timing[0] is the leader). Our own line
     * is in there too (gap 0). */
    int num_timing;
    RRTimingEntry timing[RR_MAX_CARS];
    int two_compound_rule;     /* 1: two different compounds must be used, or RR_TWO_COMPOUND_PENALTY */
    int compounds_used;        /* bit (1 << RR_TIRE_*) for each compound we have used */
    int starting_compound_set; /* 1: the team chose our starting tyres; RRRobotConfig.tire_compound is ignored */

    /* --- ABI 6 --- */
    int pits_closed;           /* 1: pit requests are ignored in this session */

    /* --- ABI 7 --- */
    /* Each tyre and brake: front left, front right, rear left, rear right.
     * Brake heat soaks into the tyres through the wheel rims, with a lag of a
     * minute or so; tire_temp is the mean of each axle's two tyres. */
    float tire_temp_wheel[4];  /* C */
    float brake_temp[4];       /* C, disc */
    float brake_temp_window[2];/* C: below it the brakes bite less, above it they fade */

    /* --- ABI 8 --- */
    int session;               /* RR_SESSION_* */
    int turn;                  /* id of the turn we are in (RRTurn.id), 0 on a straight */
    int next_turn;             /* id of the next turn ahead (the current one's successor when in a turn) */
    float next_turn_ds;        /* m along the track to next_turn's start */

    /* --- ABI 9 --- the road under the car (see RRTrackPoint3) */
    float z;                   /* road height, m */
    float grade;               /* along the track, + = uphill */
    float bank;                /* rad, + = left edge higher */

    /* --- ABI 10 --- KERS: a motor-generator on the crankshaft (kers_power). It
     * recovers energy under braking (a part of the braking is done by it) and
     * adds kers_power W to the engine while RRControl.kers is set at full
     * throttle, up to kers_energy J per lap. All zero without KERS. */
    float kers_store;          /* J in the store now */
    float kers_deploy_left;    /* J that may still be released this lap */
    float kers_harvest_left;   /* J that may still be recovered this lap */
    float kers_power;          /* W now: + deploying, - recovering */
    /* DRS: RR_DRS_*. Open it with RRControl.drs while it is RR_DRS_AVAILABLE;
     * it closes when you touch the brakes, leave the zone or let go of drs. */
    int drs_state;
    int drs_open;              /* 1 while the flap is (opening) open */
    int drs_zone;              /* index into RRTrackInfo.drs_zones of the zone we are in, -1 outside */
    int drs_next_zone;         /* the next zone ahead */
    float drs_next_ds;         /* m to the start of drs_next_zone (0 inside it) */
    float drs_gap;             /* s behind the car ahead when we last crossed a detection point, -1 if none */

    /* --- ABI 11 --- the surface under the car's centre: RR_SURF_*. Grip and drag
     * differ per surface (grass and gravel are slow, kerbs grip almost like tarmac). */
    int surface;

    /* --- ABI 12 --- wheels and track limits. Wheel order: front left, front right,
     * rear left, rear right. The white line is at +-half_width of the track point
     * (RRTrackPoint) at the wheel's own track distance, so |wheel_track_pos| > that
     * means the wheel is beyond the line.
     * Track limits (races only): when all four wheels have been beyond the white line
     * for RR_TRACK_LIMIT_DWELL seconds, that is a strike (and another every RR_TRACK_LIMIT_REPEAT s it stays out). The first RR_TRACK_LIMIT_WARNINGS
     * strikes are warnings, every later one costs RR_TRACK_LIMIT_PENALTY s. No strike
     * is given while the car is not in control: spun, sliding past the tyres' limit, or
     * within RR_TRACK_LIMIT_GRACE s of contact with a car or a barrier, nor in the pit area.
     * Kerbs and run-off count as beyond the line, as in real racing. */
    float wheel_track_pos[4];  /* m from the centreline at each wheel, + = left */
    int wheel_surface[4];      /* RR_SURF_* under each wheel */
    int wheels_outside;        /* wheels beyond the white line, 0..4 */
    int track_limit_strikes;   /* strikes so far this race */
    int track_limit_exempt;    /* 1 while no strike can be given (see above) */
    float track_limit_dwell;   /* s with all four wheels outside so far in this excursion */

    /* --- ABI 13 --- pit road and penalties to serve (see RR_PIT_SPEED_TOLERANCE) */
    float pit_speed_limit;     /* m/s, 0 when the track has no pit lane */
    int pit_zone;              /* 1 while on the pit road (entry line to exit line, pit side of the white line) */
    int pit_limiter;           /* 1 while the host's limiter is acting */
    float pit_speeding;        /* m/s over the limit right now, 0 if not over */
    float penalty_owed;        /* s of penalties still to serve in a stop (or to be added to the race time) */
    float service_penalty_left;/* s the car is held still for a penalty, while RR_PIT_SERVICE (before the crew starts) */

    /* --- ABI 14 --- yellow flags, the virtual safety car and incident holds (see RR_FLAG_*) */
    int flag_state;            /* RR_FLAG_*: the flag in the marshal sector the car is in or about to enter */
    int vsc_state;             /* RR_VSC_*: the virtual safety car, for the whole track */
    float speed_cap;           /* m/s, 0 if none: the speed the host holds this car to now (yellow, VSC, rejoin) */
    float incident_ds;         /* m ahead to the nearest flagged incident (the car in trouble), -1 if none */
    int held;                  /* 1: the host holds this car after an incident; controls are ignored until released */
} RRSensors;

#define RR_TRACK_LIMIT_WARNINGS 2
#define RR_TRACK_LIMIT_PENALTY 5.0f  /* s */
#define RR_TRACK_LIMIT_DWELL 0.25f   /* s */
#define RR_TRACK_LIMIT_REPEAT 4.0f   /* s: staying outside earns another strike this often */
#define RR_TRACK_LIMIT_GRACE 2.0f    /* s after contact, a spin or a slide */

/* --- ABI 13: pit lane rules and penalties served in the pits ---
 * Pit road: from the pit entry line (RRPitInfo.entry_s) to the exit line (exit_s), on the pit side of
 * the white line. Robots built for ABI 13 or later must keep to RRPitInfo.speed_limit there:
 *  - pit_limiter (RRControl) engages the host's limiter, which cuts the throttle and brakes above the
 *    limit; without it the car may go faster and is caught speeding. Be at the limit by the entry line
 *    (the limiter starts when the car is already inside).
 *  - Speeding: more than RR_PIT_SPEED_TOLERANCE m/s over the limit is a RR_PEN_PIT_SPEEDING time penalty
 *    (RR_PEN_PIT_SPEEDING_HIGH if more than RR_PIT_SPEED_HIGH m/s over), once per visit, escalating to
 *    the higher one.
 *  - Exit line: after the pit lane, the car's centre must stay on the pit side of the white line until
 *    RR_PIT_EXIT_MARGIN m before exit_s; crossing early is a RR_PEN_PIT_EXIT penalty.
 * Robots built for older ABIs get the limiter on automatically in the pit lane (lane_start_s to
 * lane_end_s, as before) and none of the pit road rules above.
 * Rules for every robot, in races only:
 *  - Causing a collision: a car whose nose hits another car's side or rear at a closing speed of at
 *    least RR_PEN_COLLISION_SPEED m/s gets RR_PEN_COLLISION s (RR_PEN_COLLISION_HIGH s from
 *    RR_PEN_COLLISION_HIGH_SPEED m/s). Wheel to wheel and head-on contact is a racing incident.
 * Penalties from pit speeding, the exit line and collisions are served: they are owed (penalty_owed)
 * and the next stop in the box holds the car still for that long before the crew starts, with the
 * crew's hands off the car (service_penalty_left). Whatever is still owed when the car takes the flag
 * is added to its race time. Track limits and blue flag penalties keep adding to the race time at once. */
#define RR_PIT_SPEED_TOLERANCE 0.5f   /* m/s over the limit before it counts */
#define RR_PIT_SPEED_HIGH 1.4f        /* m/s over the limit (about 5 km/h): the higher penalty */
#define RR_PIT_EXIT_MARGIN 15.0f      /* m before exit_s where the white line may be crossed */
#define RR_PEN_PIT_SPEEDING 5.0f      /* s */
#define RR_PEN_PIT_SPEEDING_HIGH 10.0f
#define RR_PEN_PIT_EXIT 5.0f
#define RR_PEN_COLLISION 5.0f
#define RR_PEN_COLLISION_HIGH 10.0f
#define RR_PEN_COLLISION_SPEED 3.0f       /* m/s closing speed */
#define RR_PEN_COLLISION_HIGH_SPEED 10.0f

#define RR_SURF_TARMAC 0
#define RR_SURF_KERB 1
#define RR_SURF_GRASS 2
#define RR_SURF_GRAVEL 3
#define RR_SURF_DIRT 4
#define RR_SURF_PIT 5      /* pit lane and the paved pit area */
#define RR_SURF_RUNOFF 6   /* paved run-off area */
#define RR_NUM_SURFACES 7

/* --- ABI 14: yellow flags, VSC, incident holds ---
 * A car that crashes hard (a wall hit over RR_INCIDENT_WALL_SPEED m/s, or car contact over
 * RR_INCIDENT_CAR_SPEED m/s), stops on the track for RR_INCIDENT_STOP_TIME s, or sits across the track
 * (angle over RR_INCIDENT_ANGLE rad for RR_INCIDENT_WRONG_TIME s) is an incident. The host holds it still
 * (brakes on, controls ignored: RRSensors.held) so it cannot drive back into traffic, and releases it once
 * the track behind it is clear. Meanwhile:
 *  - the track from RR_YELLOW_BEHIND m before the car to RR_YELLOW_AFTER m past it is yellow (double
 *    yellow if the car is on the racing surface): cars in it are held to RR_YELLOW_SPEED (RR_DYELLOW_SPEED);
 *  - the virtual safety car runs for the whole track: every car is held to RR_VSC_SPEED, and nobody may
 *    overtake. It lasts at least one lap and until no car is held, then RR_VSC_ENDING_TIME s of "VSC ending"
 *    (no cap, overtaking allowed again) before green.
 * Overtaking in a yellow zone or under the VSC is a RR_PEN_NEUTRAL_PASS s penalty (served in the pits).
 * The speed caps are enforced by the host for every robot; robots of ABI 14 and later get RR_CAP_GRACE m/s
 * over the cap before the host steps in, so they can slow down smoothly by themselves with speed_cap.
 * A car held for over RR_HOLD_MAX_TIME s, released and stuck or crashing again within RR_REJOIN_WATCH s (stuck is judged after RR_REJOIN_GRACE s), or
 * held a third time, is taken off the track (retired: "removed after incident"). */
#define RR_FLAG_GREEN 0
#define RR_FLAG_YELLOW 1
#define RR_FLAG_DOUBLE_YELLOW 2
#define RR_VSC_NONE 0
#define RR_VSC_ACTIVE 1
#define RR_VSC_ENDING 2
#define RR_INCIDENT_WALL_SPEED 8.0f   /* m/s, normal speed into a barrier */
#define RR_INCIDENT_CAR_SPEED 6.0f    /* m/s, closing speed in a car-to-car impact */
#define RR_INCIDENT_STOP_TIME 3.0f    /* s standing still on the track */
#define RR_INCIDENT_ANGLE 1.9f        /* rad: more than this across the track ... */
#define RR_INCIDENT_WRONG_TIME 1.0f   /* ... for this long, below RR_INCIDENT_MAX_SPEED, is a spin */
#define RR_INCIDENT_MAX_SPEED 25.0f   /* m/s */
#define RR_YELLOW_BEHIND 250.0f       /* m of track before the incident that is flagged */
#define RR_YELLOW_AFTER 30.0f         /* m after it */
#define RR_YELLOW_SPEED 55.0f         /* m/s cap in a yellow zone */
#define RR_DYELLOW_SPEED 35.0f        /* m/s cap in a double yellow zone */
#define RR_VSC_SPEED 40.0f            /* m/s cap under the VSC (about 145 km/h) */
#define RR_VSC_ENDING_TIME 5.0f       /* s */
#define RR_REJOIN_SPEED 25.0f         /* m/s cap for RR_REJOIN_WATCH s after a release */
#define RR_REJOIN_WATCH 10.0f         /* s */
#define RR_REJOIN_GRACE 6.0f          /* s after a release before a car counts as stuck again */
#define RR_HOLD_MAX_TIME 45.0f        /* s held before the car is taken off the track */
#define RR_CAP_GRACE 1.0f             /* m/s over the cap before the host acts, ABI 14 robots */
#define RR_NEUTRAL_SETTLE 3.0f        /* s under a cap before passing is a penalty (the field is still slowing down) */
#define RR_PEN_NEUTRAL_PASS 5.0f      /* s, served in the pits */

#define RR_BLUE_FLAG_RANGE 60.0f   /* m behind us (or 1.2 s, whichever is more) */
#define RR_BLUE_FLAG_LIMIT 8.0f    /* s of holding a lapping car up before a penalty */
#define RR_BLUE_FLAG_PENALTY 5.0f  /* s added to the race time, once per lapping car held up */

typedef struct RRControl {
    float steer;      /* -1 (full right) .. +1 (full left) */
    float accel;      /* 0 .. 1 */
    float brake;      /* 0 .. 1 */
    int gear;         /* used only when auto_gear = 0, or -1 to request reverse in auto mode */
    /* Free-form debug output: shown in the viewer HUD and written to telemetry. */
    char status[64];
    float debug[RR_NUM_DEBUG];

    /* Pit stop request. Keep pit_request set while driving to the box; the
     * crew starts work once the car stops in its box (within ~2.5 m) and
     * reads the fields below at that moment. */
    int pit_request;
    float pit_fuel;   /* litres to add (clamped to the tank) */
    int pit_tires;    /* 0 keep the tyres, or RR_TIRE_* to fit a new set */
    int pit_repair;   /* 1 to repair damage (adds time) */

    /* --- ABI 5 --- optional, for the viewer: the strategy we are planning */
    int pit_window[2];  /* laps of the next planned stop: earliest, latest (0 = no stop planned) */
    int pit_plan_tires; /* compound planned for that stop (0 = none / undecided) */

    /* --- ABI 10 --- */
    float kers;         /* 0 .. 1: share of kers_power to deploy, scaled by the throttle (0 = harvest only) */
    int drs;            /* 1: open the DRS flap (if RR_DRS_AVAILABLE) */

    /* --- ABI 13 --- */
    int pit_limiter;    /* 1: engage the pit limiter (see RR_PIT_SPEED_TOLERANCE); robots of older ABIs get it automatically */
} RRControl;

/* How a session went for this car (ABI 8). */
typedef struct RRSessionSummary {
    int session;          /* RR_SESSION_* */
    int laps_done;
    float best_lap;       /* s, 0 if none */
    float total_time;     /* s on track */
    int finished;         /* 1: took the flag (practice: reached the lap limit) */
    float lap_times[64];  /* the first 64 laps, s */
    int tire_compound;    /* fitted at the end */
    float tire_wear[2];   /* front, rear at the end */
    float fuel;           /* litres left */
} RRSessionSummary;

typedef struct RRRobotApi {
    int abi_version;      /* must be RR_ABI_VERSION */
    const char* name;
    const char* author;

    /* params: the string given with --params on the command line, "" if none
     * (by convention "key=value,key=value"). Return NULL to refuse the car. */
    void* (*create)(const RRTrackInfo* track, const RRCarSpec* car, int car_index,
                    const char* params, RRRobotConfig* config);
    /* control arrives zeroed except gear, which holds the current gear. */
    void (*drive)(void* self, const RRSensors* sensors, RRControl* control);
    void (*destroy)(void* self);
    /* Optional, may be NULL: a polyline for the viewer to draw, such as the
     * planned path. Write up to max_points (x, y) pairs into xy and return how
     * many were written. Called from the render loop, never during drive(). */
    int (*debug_path)(void* self, float* xy, int max_points);

    /* --- ABI 8 --- optional, may be NULL */
    /* Called once when this car's session ends (the flag, or the session is
     * stopped), before destroy(): the last chance to write notes to the
     * weekend memory. */
    void (*session_end)(void* self, const struct RRSessionSummary* summary);
} RRRobotApi;

typedef const RRRobotApi* (*RRRobotEntryFn)(void);
#define RR_ROBOT_ENTRY_SYMBOL "rr_robot_entry"

#ifdef __cplusplus
}
#endif

#endif /* RR_ROBOT_API_H */
