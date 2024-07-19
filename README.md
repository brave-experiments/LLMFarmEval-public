# LLMFarmEval

This is a wrapper iOS app for the [llmfarm_core](https://github.com/guinmoon/llmfarm_core.swift) Swift library, based on the [llamacpp](https://github.com/ggerganov/llama.cpp) project. It is used to evaluate the performance of llamacpp on iOS devices. The `llmfarm_core.swift` dependency has also been altered to support micro benchmarking stats.


## How to build

1. Clone the repository and its submodules

```
git clone --recurse-submodules https://github.com/brave-experiments/llmfarmeval

```

2. Open the project in Xcode

File > Open, and select the `LLMFarmEval.xcodeproj` file.

3. Build and Install the project

* Product > Build and make sure the build is successful.
* Export ipa signed application archive (`Product > Archive > Distribute App > Release Testing > Export`).
* Install your .ipa generated file to your connected phone by running `./install_ios_ipa.sh` from the `llamacpp` repo.

4. Copy the model and configuration files

Before running the project, you need to make sure that the model and configuration files are available in the app.

* Populate models to your device by running `./push_ios_models.sh` from the `llamacpp` repo. You can give the models as an argument.

**WARNING**: By default, the models are copied with iFuse, which can be painfully slow. Alternatively, you can copy models on device to the application directory through Finder, which we found to be significantly faster.


## How to run

1. Open the app on your iOS device.
2. Type the output filename (without the extension) in the first text field.
3. Type the model name (including the extension) in the second text field.
4. Press the "Run Automation" button.
5. Wait for the evaluation to finish. The output file will be saved in the app's Documents directory.
