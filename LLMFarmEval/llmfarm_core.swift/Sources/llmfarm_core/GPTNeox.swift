//
//  GPTNeoX.swift
//  Mia
//
//  Created by Byron Everson on 4/19/23.
//

import Foundation
import llmfarm_core_cpp

public class GPTNeoX: LLMBase {

    public override func llm_load_model(path: String = "", contextParams: ModelContextParams = .default, params:gpt_context_params ) throws -> Bool{
        self.context = gpt_neox_init_from_file(path, params)
        if self.context == nil {
            return false
        }
        return true
    }
    
    deinit {
        gpt_neox_free(context)
    }
    
    public override func llm_eval(inputBatch:[ModelToken]) throws -> Bool{
        if gpt_neox_eval(context, inputBatch, Int32(inputBatch.count), nPast, contextParams.numberOfThreads) != 0 {
            throw ModelError.failedToEval
        }
        return true
    }
    
    
}


