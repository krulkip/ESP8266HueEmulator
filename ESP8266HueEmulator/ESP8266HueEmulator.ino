/**
 * Emulate Philips Hue Bridge ; so far the Hue app finds the emulated Bridge and gets its config
 * and switch NeoPixels with it
 **/

#include <ESP8266WiFi.h>
#include <ESP8266mDNS.h>
#include <WiFiUdp.h>
#include <ArduinoOTA.h>
#include <NeoPixelBus.h>
#include <NeoPixelAnimator.h> // instead of NeoPixelAnimator branch
#include "LightService.h"

// these are only used in LightHandler.cpp, but it seems that the IDE only scans the .ino and real libraries for dependencies
#include <ESP8266WebServer.h>
#include "SSDP.h"
#include <aJSON.h> // Replace avm/pgmspace.h with pgmspace.h there and set #define PRINT_BUFFER_LEN 4096 ################# IMPORTANT

#include "/secrets.h" // Delete this line and populate the following
//const char* ssid = "********";
//const char* password = "********";

RgbColor red = RgbColor(COLOR_SATURATION, 0, 0);
RgbColor green = RgbColor(0, COLOR_SATURATION, 0);
RgbColor white = RgbColor(COLOR_SATURATION);
RgbColor black = RgbColor(0);
unsigned int transitionTime = 800; // by default there is a transition time to the new state of 400 milliseconds

// Settings for the NeoPixels
#define pixelCount 30
// strip attached to GPIO3 - RX on wemos D1 mini
#define pixelPin 3 // Strip is attached to GPIO2 on ESP-01		+// strip attached to GPIO3 - RX on wemos D1 mini
NeoPixelBus<NeoGrbFeature, NeoEsp8266Uart800KbpsMethod> strip(pixelCount, pixelPin);
//NeoPixelBus<NeoRgbFeature, Neo800KbpsMethod> strip(pixelCount);
NeoPixelAnimator animator(pixelCount, NEO_MILLISECONDS); // NeoPixel animation management object

HsbColor getHsb(int hue, int sat, int bri) {
  float H, S, B;
  H = ((float)hue) / 182.04 / 360.0;
  S = ((float)sat) / COLOR_SATURATION;
  B = ((float)bri) / COLOR_SATURATION;
  return HsbColor(H, S, B);
}

class PixelHandler : public LightHandler {
  private:
    HueLightInfo _info;
    int16_t colorloopIndex = -1;
  public:
    void handleQuery(int lightNumber, HueLightInfo newInfo, aJsonObject* raw) {
      // define the effect to apply, in this case linear blend
      HslColor newColor = HslColor(getHsb(newInfo.hue, newInfo.saturation, newInfo.brightness));
      HslColor originalColor = strip.GetPixelColor(lightNumber);
      _info = newInfo;

      // cancel colorloop if one is running
      if (colorloopIndex >= 0) {
        animator.StopAnimation(colorloopIndex);
        colorloopIndex = -1;
      }
      if (newInfo.on)
      {
        if (_info.effect == EFFECT_COLORLOOP) {
          //color loop at max brightness/saturation on a 60 second cycle
          const int SIXTY_SECONDS = 60000;
          animator.StartAnimation(lightNumber, SIXTY_SECONDS, [ = ](const AnimationParam & param) {
            // save off animation index
            colorloopIndex = param.index;

            // progress will start at 0.0 and end at 1.0
            float currentHue = newColor.H + param.progress;
            if (currentHue > 1) currentHue -= 1;
            HslColor updatedColor = HslColor(currentHue, newColor.S, newColor.L);
            RgbColor currentColor = updatedColor;
            strip.SetPixelColor(lightNumber, updatedColor);

            // loop the animation until canceled
            if (param.state == AnimationState_Completed) {
              // done, time to restart this position tracking animation/timer
              animator.RestartAnimation(param.index);
            }
          });
          return;
        }
        AnimUpdateCallback animUpdate = [ = ](const AnimationParam & param)
        {
          // progress will start at 0.0 and end at 1.0
          HslColor updatedColor = HslColor::LinearBlend<NeoHueBlendShortestDistance>(originalColor, newColor, param.progress);
          strip.SetPixelColor(lightNumber, updatedColor);
        };
        animator.StartAnimation(lightNumber, transitionTime, animUpdate);
      }
      else
      {
        AnimUpdateCallback animUpdate = [ = ](const AnimationParam & param)
        {
          // progress will start at 0.0 and end at 1.0
          HslColor updatedColor = HslColor::LinearBlend<NeoHueBlendShortestDistance>(originalColor, black, param.progress);
          strip.SetPixelColor(lightNumber, updatedColor);
        };
        animator.StartAnimation(lightNumber, transitionTime, animUpdate);
      }
    }

    HueLightInfo getInfo(int lightNumber) { return _info; }
};

void setup() {
  Serial.begin(115200);// needs to be before strip.Begin() to allow NeoPixel DMA RX pin to work
  // this resets all the neopixels to an off state
  strip.Begin();
  strip.Show();

  // Show that the NeoPixels are alive
  delay(120); // Apparently needed to make the first few pixels animate correctly


  WiFi.mode(WIFI_STA);
  WiFi.begin(ssid, password);
  infoLight(white);

  if (WiFi.status() != WL_CONNECTED) {
    // Show that we are connecting
    infoLight(red);
    Serial.print(".");
  }
  
  // Port defaults to 8266
  // ArduinoOTA.setPort(8266);

  // Hostname defaults to esp8266-[ChipID]
  // ArduinoOTA.setHostname("myesp8266");

  // No authentication by default
  // ArduinoOTA.setPassword((const char *)"123");

  ArduinoOTA.onStart([]() {
    Serial.println("Start");
  });
  ArduinoOTA.onEnd([]() {
    Serial.println("\nEnd");
  });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR) Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR) Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR) Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR) Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR) Serial.println("End Failed");
  });
  ArduinoOTA.begin();
  
  // Show that we are connected
  infoLight(green);

  LightService.begin();

  // setup pixels as lights
  for (int i = 0; i < MAX_LIGHT_HANDLERS && i < pixelCount; i++) {
    LightService.setLightHandler(i, new PixelHandler());
  }
}

void loop() {
  ArduinoOTA.handle();
  LightService.update();

  static unsigned long update_strip_time = 0;  //  keeps track of pixel refresh rate... limits updates to 33 Hz
  if (millis() - update_strip_time > 30)
  {
    if ( animator.IsAnimating() ) animator.UpdateAnimations();
    strip.Show();
    update_strip_time = millis();
  }
}

void infoLight(RgbColor color) {
  // Flash the strip in the selected color. White = booted, green = WLAN connected, red = WLAN could not connect
  for (int i = 0; i < pixelCount; i++)
  {
    strip.SetPixelColor(i, color);
    strip.Show();
    delay(10);
    strip.SetPixelColor(i, black);
    strip.Show();
  }
}
