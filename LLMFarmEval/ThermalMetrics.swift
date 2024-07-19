//
//  ThermalMetrics.swift
//  LLMFarmEval
//
//  Created by Kleomenis Katevas on 29/05/2024.
//

import Foundation

struct ThermalState: Codable {
    let timestamp: Date
    let state: String
}

class ThermalMetricsRecordManager: ObservableObject {
    @Published private var thermalStates: [ThermalState] = []

    func addThermalState(_ thermalState: ThermalState) {
        thermalStates.append(thermalState)
    }

    func saveToFile(withFileName fileName: String) {
        let fileManager = FileManager.default
        let directoryURL = fileManager.urls(for: .documentDirectory, in: .userDomainMask)[0]
        let fileURL = directoryURL.appendingPathComponent("\(fileName).json")
        
        let encoder = JSONEncoder()
        encoder.outputFormatting = .prettyPrinted
        encoder.dateEncodingStrategy = .custom { (date, encoder) in
            var container = encoder.singleValueContainer()
            let timestamp = date.timeIntervalSince1970
            try container.encode(timestamp)
        }
        
        do {
            let data = try encoder.encode(thermalStates)
            try data.write(to: fileURL)
            print("Energy measurements JSON file successfully saved at \(fileURL)")
        } catch {
            print("Failed to write JSON data: \(error.localizedDescription)")
        }
    }
}
