import Foundation
import Vision

let request = VNRecognizeTextRequest()
request.recognitionLevel = .accurate
request.usesLanguageCorrection = false
try VNImageRequestHandler(url: URL(fileURLWithPath: CommandLine.arguments[1])).perform([request])
for observation in request.results ?? [] {
    if let first = observation.topCandidates(1).first {
        print(first.string)
    }
}
