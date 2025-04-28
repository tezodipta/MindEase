# MindEase

## Project Description
MindEase is a mental health assistant designed to provide emotional support and advice to users. It leverages an ESP32-based hardware setup for audio input/output and a fine-tuned AI model for natural language understanding and response generation. The project integrates hardware, AI, and backend services to create a seamless user experience.

## Use of This Project
MindEase can be used to:
- Provide mental health support and advice.
- Act as a conversational assistant for emotional well-being.
- Demonstrate the integration of IoT devices with AI models.

## Hardware Requirements
To build the hardware setup, you will need:
- **ESP32 Development Board** (e.g., ESP32-DevKitC).
- **INMP441 Microphone** for audio input.
- **MAX98357A Amplifier** for audio output.
- **LEDs** for status indication:
  - WiFi connection status.
  - Audio recording status.
- **Push Button** for triggering the assistant.
- **Power Supply** (e.g., USB or battery).

### Circuit Connections
1. **Microphone (INMP441):**
   - `LRC` → GPIO 5
   - `DOUT` → GPIO 19
   - `BCLK` → GPIO 18

2. **Amplifier (MAX98357A):**
   - `DIN` → GPIO 22
   - `BCLK` → GPIO 15
   - `LRC` → GPIO 21

3. **LEDs:**
   - WiFi Status LED → GPIO 25
   - Audio Recording LED → GPIO 32
   - Built-in LED → GPIO 2 (optional)

4. **Push Button:**
   - Connect to GPIO 4 with a pull-up resistor.

5. **Power Supply:**
   - Connect the ESP32 to a 5V power source or use a Power Bank.

## Backend Setup (Node.js)
1. Install [Node.js](https://nodejs.org/) and [Python](https://www.python.org/).
2. Clone this repository:
   ```bash
   git clone https://github.com/your-repo/MindEase.git
   cd MindEase
   ```
3. Navigate to the `Backend` folder:
   ```bash
   cd Backend
   ```
4. Install dependencies:
   ```bash
   npm install
   ```
5. Configure environment variables:
   - Create a `.env` file in the `Backend` folder.
   - Add the following variables:
     ```
     PORT=3000
     GOOGLE_APPLICATION_CREDENTIALS=path/to/google_creds.json
     GROQ_API_KEY=your_groq_api_key
     ```
6. Start the backend server:
   ```bash
   npm start
   ```

## Backend Setup (Flask with ngrok)
1. Install Python dependencies:
   ```bash
   pip install fastapi uvicorn pyngrok transformers torch accelerate
   ```
2. Start the Flask backend:
   ```bash
   python app.py
   ```
3. Use ngrok to expose the local server:
   ```bash
   ngrok http 8000
   ```
4. Copy the public URL provided by ngrok and use it to access the backend API.
5. I'm useing my own address for the backend, you can use your own address for the backend. 
   

## Using Your Own Model
To use your own fine-tuned model:
1. Fine-tune the model using the provided notebook (`Model/Model_train_colab.ipynb`or`Model/Model_train_local.ipynb`).
2. Save the fine-tuned model in the `fine_tuned_model` folder.
   or download the model from my drive link: "[Drive Link](https://drive.google.com/drive/folders/11VeRhBe8cPlYCW4i28w7onL91DcEWnJX?usp=sharing)
3. Update the backend and ESP32 code to point to your model.

## Accessing the Model via API
You can use your Own api to access the model from local or use the provided `MindEase_AI_backend.ipynb` file to expose the model as an API. This API can be accessed from platforms like Google Colab or other applications.

## License
This project is licensed under the [MIT License](https://opensource.org/licenses/MIT). You are free to use, modify, and distribute this project, provided proper attribution is given.

## Acknowledgments
- [Hugging Face](https://huggingface.co/) for providing pre-trained models.
- [Google Cloud](https://cloud.google.com/) for Speech-to-Text and Text-to-Speech APIs.
- [ngrok](https://ngrok.com/) for exposing local servers to the internet.
- [PlatformIO](https://platformio.org/) for ESP32 development.

Feel free to contribute to this project by submitting issues or pull requests!
