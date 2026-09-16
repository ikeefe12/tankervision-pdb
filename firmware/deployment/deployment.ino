// Runtime is in Application.cpp; this keeps the sketch folder independently buildable.
void setupDeployment();
void loopDeployment();
void setup() { setupDeployment(); }
void loop() { loopDeployment(); }
