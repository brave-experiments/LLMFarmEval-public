//
//  ViewController.swift
//  LLMFarmEval
//
//  Created by Kleomenis Katevas on 28/09/2023.
//

import UIKit
import llmfarm_core

class ViewController: UIViewController {
    
    @IBOutlet weak var actionButton: UIButton!
    @IBOutlet weak var filenameTextField: UITextField!
    @IBOutlet weak var modelTextField: UITextField!
    @IBOutlet weak var textView: UITextView!
    
    let thermalMetricsRecordManager = ThermalMetricsRecordManager()

    override func viewDidLoad() {
        super.viewDidLoad()
        // Do any additional setup after loading the view.
        
        RestAwaitLib.requestPermission()
        
        // Add observer for thermal state changes and force initial state
        NotificationCenter.default.addObserver(self, selector: #selector(handleThermalStateChange), name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
        handleThermalStateChange()
        
        DispatchQueue.main.asyncAfter(deadline: .now()) {
            self.filenameTextField.becomeFirstResponder()
        }
    }
    
    deinit {
        // Remove observer when the view controller is deallocated
        NotificationCenter.default.removeObserver(self, name: ProcessInfo.thermalStateDidChangeNotification, object: nil)
    }
    
    @objc func handleThermalStateChange() {
        
        let thermalStateEnum = ProcessInfo.processInfo.thermalState
        
        // 0. nominal: No corrective action is needed.
        // 1. fair: The system has reached a state where fans may become audible (on systems which have fans). Recommendation: Defer non-user-visible activity.
        // 2. serious: Fans are running at maximum speed (on systems which have fans), system performance may be impacted. Recommendation: reduce application's usage of CPU, GPU and I/O, if possible. Switch to lower quality visual effects, reduce frame rates.
        // 3. critical: System performance is significantly impacted and the system needs to cool down. Recommendation: reduce application's usage of CPU, GPU, and I/O to the minimum level needed to respond to user actions. Consider stopping use of camera and other peripherals if your application is using them.
        
        var state: String
        
        switch thermalStateEnum {
        case .nominal:
            state = "nominal"
        case .fair:
            state = "fair"
        case .serious:
            state = "serious"
        case .critical:
            state = "critical"
        @unknown default:
            state = "@unknown"
        }
        
        // add state
        let thermalState = ThermalState(timestamp: Date(), state: state)
        thermalMetricsRecordManager.addThermalState(thermalState)
    }
    
    func getFileURLFromName(_ name: String) -> URL {
        let fileManager = FileManager.default
        let documentsPath = fileManager.urls(for: .documentDirectory, in: .userDomainMask).first
        let destinationURL = documentsPath!.appendingPathComponent(name)
        return destinationURL
    }
    
    @IBAction func terminateAppAction(_ sender: Any) {
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) {
            UIApplication.shared.perform(#selector(NSXPCConnection.suspend))
             DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) {
              exit(0)
             }
        }
    }

    @IBAction func buttonAction(_ sender: Any) {
        
        actionButton.isEnabled = false
        actionButton.setTitle("Running...", for: .normal)
        self.filenameTextField.isEnabled = false
        self.modelTextField.isEnabled = false
        self.textView.text += "-Started-\n"
        
        let modelFilename = self.modelTextField.text!
        let measurementFilename = self.filenameTextField.text?.isEmpty ?? true ? "measurements" : self.filenameTextField.text!
        
        DispatchQueue.global(qos: .userInitiated).async {
            
            self.runAutomation(withModel: modelFilename, withMeasurement: measurementFilename)
            
            DispatchQueue.main.async {
                self.actionButton.setTitle("Run Automation", for: .normal)
                self.actionButton.isEnabled = true
                self.filenameTextField.isEnabled = true
                self.modelTextField.isEnabled = true
                self.textView.text += "-Finished-\n"
            }
        }
    }
    
    func runAutomation(withModel modelFilename: String, withMeasurement measurementFilename: String) {
        
        let conversationsRecordManager = ConversationsRecordManager()
        let conversations = readInputFile()
        let config = readModelConfiguration()
        let generation = config["generation"] as! [String: Any]
        let maxOutputLength = generation["max_gen_len"] as! Int
        
        // per conversation
        for (c_idx, conversation) in conversations.enumerated() {
            
            var conversationRecord = ConversationRecord(modelName: modelFilename)
            
            // load model
            let modelTimeStart = Date()
                
            let model = loadModel(withName: modelFilename, withConfig: config)
                
            let modelDuration = -modelTimeStart.timeIntervalSinceNow
            conversationRecord.modelLoadTime = TimeRecord(start: modelTimeStart, duration: modelDuration)
            sleep(5)
            
            // per question in the conversation
            for (q_idx, question) in conversation.enumerated() {
                
                //print(question)
                
                DispatchQueue.main.async {
                    self.textView.text += "\(c_idx)_\(q_idx) Prompt: \(question)\n"
                }
                
                let timeStart = Date()
                
                let prediction = predict(question, withModel: model, withMaxOutputLength: maxOutputLength)
                let original_session_tokens = prediction.original_session_tokens
                let input_tokens = prediction.input_tokens
                let output_tokens = prediction.output_tokens
                
                let questionRecord = QuestionRecord.init(time: TimeRecord(start: timeStart, duration: -timeStart.timeIntervalSinceNow),
                                                 input: question,
                                                 output: prediction.output!,
                                                 original_session_tokens: original_session_tokens,
                                                 input_tokens: input_tokens,
                                                 output_tokens: output_tokens,
                                                 runtimeStats: "")
                conversationRecord.questionRecords.append(questionRecord)
                
                DispatchQueue.main.async {
                    self.textView.text += "\(c_idx)_\(q_idx) Answer: \(prediction.output!)\n"
                }
                
                sleep(5)
            }
            
            // Save events for particular session
            model.model?.saveEnergyEventsToCSV(withFileName: "\(measurementFilename)_conv\(c_idx)")
            model.model?.saveTimingEventsToCSV(withFileName: "\(measurementFilename)_timing_conv\(c_idx)")
            
            // add metrics
            conversationsRecordManager.addConversationRecord(conversationRecord)
            DispatchQueue.main.async {
                self.textView.text += "--sleep--\n"
            }
            sleep(60)
        }
        
        // Add session and save
        conversationsRecordManager.saveToFile(withFileName: measurementFilename)
        
        // Save thermal metrics
        thermalMetricsRecordManager.saveToFile(withFileName: "\(measurementFilename)_thermals")
        
        // Notify BladeRunner that task is complete
        let restAwaitLib = RestAwaitLib(host: "192.168.1.42", port: 5100)
        restAwaitLib.continueExecution { response, error in
            if (response != nil) {
                print(response!)
            }
            else {
                print(error!)
            }
        }
    }
    
    func loadModel(withName name: String, withConfig config:[String: Any]) -> AI {
        let modelPath = getFileURLFromName(name).path
        let ai = AI(_modelPath: modelPath, _chatName: "chat")
        
        // context params
        let generation = config["generation"] as! [String: Any]
        let prompt = config["prompt"] as! [String: Any]
        let prompt_in_prefix = (prompt["in_prefix"] as? String) ?? ""
        let prompt_text = prompt["text"] as! String
        let prompt_in_suffix = (prompt["in_suffix"] as? String) ?? ""
        
        var params:ModelAndContextParams = .default
        params.n_threads = generation["n_threads"] as? Int32 ?? Int32(ProcessInfo.processInfo.processorCount)
        params.use_metal = (generation["use_metal"] as? Bool) ?? true
        params.useMlock = (generation["useMlock"] as? Bool) ?? false
        params.useMMap = (generation["useMMap"] as? Bool) ?? true
        params.context = generation["max_window_size"] as! Int32
        params.promptFormat = .Custom
        params.custom_prompt_format = "\(prompt_in_prefix)\(prompt_text){{prompt}}\(prompt_in_suffix)"
        params.reverse_prompt = prompt["reverse"] as? String != nil ? [prompt["reverse"] as! String] : []
        
        let modelInference = ModelInference.LLama_gguf
        
        ai.initModel(modelInference, contextParams: params)
        if ai.model == nil{
            print( "Model load eror.")
            exit(2)
        }
        
        // sample Params
        let sampling = config["sampling"] as! [String: Any]
        ai.model?.sampleParams.n_batch = sampling["n_batch"] as! Int32
        ai.model?.sampleParams.repeat_last_n = sampling["repeat_last_n"] as! Int32
        ai.model?.sampleParams.repeat_penalty = Float(sampling["repetition_penalty"] as! Double)
        ai.model?.sampleParams.temp = Float(sampling["temperature"] as! Double)
        ai.model?.sampleParams.top_k = sampling["top_k"] as! Int32
        ai.model?.sampleParams.top_p = Float(sampling["top_p"] as! Double)
        
        do {
            try ai.loadModel_sync()
        }
        catch {
            print("Error info: \(error)")
        }

        return ai
    }
    
    func predict(_ input_text: String, withModel model: AI, withMaxOutputLength maxOutputLength: Int) -> (output: String?, original_session_tokens: Int, input_tokens: Int, output_tokens: Int) {
        
        //print("Prompt: \(input_text)")
        
        var total_output = 0
        var output_tokens = 0

        func mainCallback(_ str: String, _ time: Double) -> Bool {
            //print("\(str)" ,terminator: "")
            total_output += str.count
            output_tokens += 1
            if(total_output>maxOutputLength){
                return true
            }
            return false
        }
        
        let model = model.model!  // force the unwrap
        
        let original_session_tokens = model.session_tokens.count
        
        let output = try? model.predict(input_text, mainCallback)
        
        let total_session_tokens = model.session_tokens.count - original_session_tokens
        let input_tokens = total_session_tokens - output_tokens
        
        print("original_session_tokens: \(original_session_tokens)")
        print("total_session_tokens: \(total_session_tokens)")
        print("input_tokens: \(input_tokens)")
        print("output_tokens: \(output_tokens)")

        //print(output)
        return (output, original_session_tokens, input_tokens, output_tokens)
    }
    
    // read input.json and return [[questions]] (conversation with questions)
    func readInputFile() -> [[String]] {
        
        let url = getFileURLFromName("input.json")
        do {
            // Load the data from the file into a Data object
            let data = try Data(contentsOf: url)
            
            // Decode the JSON data
            let jsonDecoder = JSONDecoder()
            let questions = try jsonDecoder.decode([[String]].self, from: data)
            
            return questions
            
        } catch {
            print("Error reading or decoding file: \(error)")
            return []
        }
    }
    
    // read model_config.json file
    func readModelConfiguration() -> [String: Any] {
        let url = getFileURLFromName("model_config.json")
        do {
            // Load the data from the file into a Data object
            let data = try Data(contentsOf: url)
            
            // Decode the JSON data
            let jsonObject = try JSONSerialization.jsonObject(with: data, options: [])
            
            if let configuration = jsonObject as? [String: Any] {
                return configuration
            } else {
                print("Invalid JSON format")
                return [:]
            }
            
        } catch {
            print("Error reading or decoding file: \(error)")
            return [:]
        }
    }
}
