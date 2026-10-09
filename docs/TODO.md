# To do

Physics and environment, in the order agreed:

1. **Separate air and track temperature.** `--ambient` is one number for both today. The track surface runs
   10-25 C above the air in sun. Track temperature should drive tyre heating, tyre grip and the cooling of
   tyres and brakes on the road side; air temperature should drive engine cooling and (later) air density.
2. **Rubber on track (track evolution).** Grip builds up along the racing line as cars lay rubber, and falls
   away off the line. Over a weekend that is about 1 s in real F1. Needs a per-track grip map along s and
   lateral position that cars write to and read from.
3. **Engine temperature model.** Water and oil temperature, cooling from airflow (so following closely and
   a hot day cost something), power loss when overheating. There is no engine temperature in the sim today.
4. **Brake temperature model, checked.** Brake temperatures already exist (carbon discs, 350-1000 C window,
   fade above it, poor bite below it, and heat soaking disc -> rim -> tyre). Review that the brake force
   really follows disc temperature in every case, that the heat path to the tyres is realistic (rim
   temperatures about 100-250 C), and that cooling ducts and speed behave sensibly.
5. **Redo the team stats (development tokens) for the new specs.** `specs/development.json` and `--dev`
   were built around the old car. Rework them for the 2013 car and its new systems (KERS store and
   deployment, DRS, engine and brake temperature, track evolution), then balance the cost and effect of
   every stat so each one matters somewhat and no single setup is best for every bot and driving style.
   Stats should favour styles: for example top speed and drag for slipstream and DRS overtakers, downforce
   and handling for corner speed, tyre and brake care for long stints, KERS capacity and efficiency for
   energy-managed laps. Check it by racing varied bots on varied tracks with each setup.

Not planned (judged too small to matter): ride-height aero, camber, tyre pressure, suspension geometry,
MGU-H.
