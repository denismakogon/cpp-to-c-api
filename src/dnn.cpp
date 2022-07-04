//
//  dnn.cpp
//  c_api
//
//  Created by Denis Makogon on 23.06.2022.
//
#include <iostream>
#include <cstdio>
#include <iterator>
#include <fstream>
#include <vector>

#include "opencv2/core/mat.hpp"
#include "opencv2/core/utility.hpp"
#include "opencv2/imgproc.hpp"
#include "opencv2/imgcodecs.hpp"
#include "opencv2/videoio.hpp"
#include "opencv2/dnn.hpp"
#include "opencv2/objdetect.hpp"


#include "include/debug.hpp"
#include "include/etc.hpp"
#include "include/export.hpp"
#include "include/image.hpp"
#include "include/video.hpp"
#include "include/files.hpp"
#include "include/to_string.hpp"

#include "include/print.hpp"

#include "include/data_types.h"

using namespace std;
using namespace cv;
using namespace cv::samples;
using namespace cv::dnn;

/*-----------------------------------------------------------------------*/
/*-------------------------------DNN API---------------------------------*/
/*-----------------------------------------------------------------------*/

int setupDNN(string modelWeights, Net& net,
             int backend=DNN_BACKEND_DEFAULT,
             int target=DNN_TARGET_CPU) {
    debug("in setupDNN");
    try {
        net = readNet(findFile(modelWeights));
        net.setPreferableBackend(backend);
        net.setPreferableTarget(target);
    } catch (Exception& ex) {
        debug(ex.what());
        return -1;
    }
    debug(format("done with setupDNN, retCode: %d", 0));
    return 0;
}


int setupDNN(string modelPath, string modelWeights, Net& net,
             int backend=DNN_BACKEND_DEFAULT,
             int target=DNN_TARGET_CPU) {
    debug("in setupDNN");
    try {
        net = readNetFromDarknet(findFile(modelPath), findFile(modelWeights));
        net.setPreferableBackend(backend);
        net.setPreferableTarget(target);
    } catch (Exception& ex) {
        debug(ex.what());
        return -1;
    }
    debug(format("done with setupDNN, retCode: %d", 0));
    return 0;
}

void inputPreprocessYOLOv5(const Mat& frame, Net& net, Size inputSize, float scale,
                           const Scalar& mean, bool swapRB) {
    debug("in inputPreprocessYOLOv5");
    int col = frame.cols;
    int row = frame.rows;
    int _max = MAX(col, row);
    cv::Mat resized = cv::Mat::zeros(_max, _max, CV_8UC3);
    frame.copyTo(resized(cv::Rect(0, 0, col, row)));
    
    Mat blob;
    blobFromImage(frame, blob, 1./255., inputSize, cv::Scalar(), true, false);
    net.setInput(blob);
    debug("done with inputPreprocessYOLOv5");
}

void inputPreprocessYOLOv3(const Mat& frame, Net& net, Size inputSize, float scale,
                       const Scalar& mean, bool swapRB) {
    debug("in inputPreprocessYOLOv3");
    static Mat blob;
    if (inputSize.width <= 0) inputSize.width = frame.cols;
    if (inputSize.height <= 0) inputSize.height = frame.rows;
    blobFromImage(frame, blob, 1.0, inputSize, Scalar(), swapRB, false, CV_8U);
    net.setInput(blob, "", scale, mean);
    if (net.getLayer(0)->outputNameToIndex("im_info") != -1)  // Faster-RCNN or R-FCN
    {
        resize(frame, frame, inputSize);
        Mat imInfo = (Mat_<float>(1, 3) << inputSize.height, inputSize.width, 1.6f);
        net.setInput(imInfo, "im_info");
    }
    debug("done with inputPreprocessYOLOv3");
}

void inputPreprocess(const Mat& frame, Net& net, Size inputSize, float scale,
                       const Scalar& mean, bool swapRB) {
    debug("in inputPreprocess");
    if (inputSize.width > 320) {
        inputPreprocessYOLOv5(frame, net, inputSize, scale, mean, swapRB);
    } else {
        inputPreprocessYOLOv3(frame, net, inputSize, scale, mean, swapRB);
    }
    debug("done with inputPreprocess");
}

void formatDetections(Mat& frame, vector<Mat>& outs, Net& net, vector<ObjectDetectionDescriptor>& ds,
                      vector<string>& cocoClasses,
                      double confidenceThresholdMax=1.0,
                      double confidenceThresholdMin=0.1,
                      int inputSize=640) {
    debug("in formatDetections");
    vector<int> outLayers = net.getUnconnectedOutLayers();
    string outLayerType = net.getLayer(outLayers[0])->type;
        
    if (outLayerType == "DetectionOutput") {
        for (size_t k = 0; k < outs.size(); k++) {
            float* data = (float*)outs[k].data;
            for (size_t i = 0; i < outs[k].total(); i += 7) {
                float confidence = data[i + 2];
                int classID = (int)(data[i + 1]) - 1;
                
                checkElementByIndexAt(cocoClasses, classID);
                bool isMatch =
                    (confidence > confidenceThresholdMin && confidence < confidenceThresholdMax) && checkElementByIndexAt(cocoClasses, classID);
                
                if (isMatch) {
                    int left   = (int)data[i + 3];
                    int top    = (int)data[i + 4];
                    int right  = (int)data[i + 5];
                    int bottom = (int)data[i + 6];
                    int width  = right - left + 1;
                    int height = bottom - top + 1;
                    if (width <= 2 || height <= 2) {
                        left   = (int)(data[i + 3] * frame.cols);
                        top    = (int)(data[i + 4] * frame.rows);
                        right  = (int)(data[i + 5] * frame.cols);
                        bottom = (int)(data[i + 6] * frame.rows);
                        width  = right - left + 1;
                        height = bottom - top + 1;
                    }
                    
                    ds.push_back((ObjectDetectionDescriptor) {
                        .className = strdup(cocoClasses[classID].c_str()),
                        .confidence = confidence,
                        .rect = (ExportableRectangle) {
                            .x0 = left,
                            .y0 = top,
                            .x1 = left + width,
                            .y1 = top + height,
                            .width = width,
                            .height = height
                        }
                    });
                }
            }
        }
    } else if (outLayerType == "Region") {
        for (long i = 0; i < outs.size(); ++i) {
            float* data = (float*)outs[i].data;
            for (int j = 0; j < outs[i].rows; ++j, data += outs[i].cols) {
                Mat scores = outs[i].row(j).colRange(5, outs[i].cols);
                Point classIdPoint;
                double confidence;
                minMaxLoc(scores, 0, &confidence, 0, &classIdPoint);

                bool isMatch =
                    (confidence > confidenceThresholdMin && confidence < confidenceThresholdMax) && checkElementByIndexAt(cocoClasses, classIdPoint.x);

                if (isMatch) {
                    int centerX = (int)(data[0] * frame.cols);
                    int centerY = (int)(data[1] * frame.rows);
                    int width = (int)(data[2] * frame.cols);
                    int height = (int)(data[3] * frame.rows);
                    
                    int left = centerX - width / 2;
                    int top = centerY - height / 2;
                    
                    ObjectDetectionDescriptor ddd = (ObjectDetectionDescriptor) {
                        .className = strdup(cocoClasses[classIdPoint.x].c_str()),
                        .confidence = confidence,
                        .rect = (ExportableRectangle) {
                            .x0 = left,
                            .y0 = top,
                            .x1 = left + width,
                            .y1 = top + height,
                            .width = width,
                            .height = height
                        }
                    };
                    ds.push_back(ddd);
                }
            }
        }
    } else if (outLayerType == "Concat") {
        int index = 0;
        float *data = (float*) outs[0].data;
        const int dimensions = 85;
        const int rows = 25200;
        for (int i = 0; i < rows; ++i) {
            float confidence = data[4];
            float * classes_scores = data + 5;
            Mat scores(1, static_cast<int>(cocoClasses.size()), CV_32FC1, classes_scores);
            Point classIdPoint;
            double max_class_score;
            minMaxLoc(scores, 0, &max_class_score, 0, &classIdPoint);

            bool isMatch =
                (confidence > confidenceThresholdMin && confidence < confidenceThresholdMax) &&
                    checkElementByIndexAt(cocoClasses, classIdPoint.x);

            if (isMatch) {
                int width = data[2];
                int height = data[3];
                int left = data[0] - width / 2;
                int top = data[1] - height / 2;
                
                ObjectDetectionDescriptor ddd = (ObjectDetectionDescriptor) {
                    .className = strdup(cocoClasses[classIdPoint.x].c_str()),
                    .confidence = confidence,
                    .rect = (ExportableRectangle) {
                        .x0 = left,
                        .y0 = top,
                        .x1 = left + width,
                        .y1 = top + height,
                        .width = width,
                        .height = height
                    }
                };
                index++;
                debug(toString(ddd));
                ds.push_back(ddd);
            }
            data += dimensions;
        }
    } else
        CV_Error(Error::StsNotImplemented, "Unknown output layer type: " + outLayerType);

    debug("done with formatDetections");
}

void runObjectDetectionsOn(Mat& img, Net& net, vector<ObjectDetectionDescriptor>& ds,
                           vector<string>& cocoClasses,
                           double confidenceThresholdMin=0.1,
                           double confidenceThresholdMax=1.0,
                           int inputSize=640) {
    debug("in runObjectDetectionsOn");
    vector<Mat> outputs;
    
    inputPreprocess(img, net, Size(inputSize, inputSize), (float) 1/255, Scalar(0, 0, 0), false);
    net.forward(outputs, net.getUnconnectedOutLayersNames());
    
    formatDetections(img, outputs, net, ds, cocoClasses,
                     confidenceThresholdMax=confidenceThresholdMax,
                     confidenceThresholdMin=confidenceThresholdMin,
                     inputSize=inputSize);
    debug(format("in runObjectDetectionsOn, detections number = %lu", ds.size()));
    
    debug("done with runObjectDetectionsOn");
}

void _runObjectDetectionsOn(Mat& img, Net& net, vector<ObjectDetectionDescriptor>& ds,
                           vector<string>& cocoClasses,
                           double confidenceThresholdMin,
                           double confidenceThresholdMax,
                           int inputSize) {
    runObjectDetectionsOn(img, net, ds, cocoClasses,
                          confidenceThresholdMin=confidenceThresholdMin,
                          confidenceThresholdMax=confidenceThresholdMax,
                          inputSize=inputSize);
}


int _runDetectionsOnImage(Net& net, string imagePath, string cocoClassesFilePath,
                          vector<ObjectDetectionDescriptor>& ds,
                          double confidenceThresholdMin=0.1,
                          double confidenceThresholdMax=1.0,
                          int inputSize=640) {
    debug("in _runDetectionsOnImage");
    Mat frame;
    int retCode;
    
    retCode = readImageFile(frame, imagePath, IMREAD_COLOR);
    if ( retCode != 0 ) {
        return retCode;
    }

    vector<string> cocoClasses;
    retCode = readFileToVectorOf(cocoClassesFilePath, cocoClasses);
    if ( retCode != 0 ) {
        return retCode;
    }
    
    runObjectDetectionsOn(frame, net, ds, cocoClasses,
                          confidenceThresholdMin=confidenceThresholdMin,
                          confidenceThresholdMax=confidenceThresholdMax,
                          inputSize=inputSize);

    debug(format("in _runDetectionsOnImage, detections number = %lu", ds.size()));

    debug(format("done with _runDetectionsOnImage, retCode: %d", retCode));
    return retCode;
}

int runDetectionsOnImageONNX(string imagePath, string modelWeights,
                             string cocoClassesFilePath,
                             vector<ObjectDetectionDescriptor>& ds,
                             double confidenceThresholdMin=0.1,
                             double confidenceThresholdMax=1.0,
                             int inputSize=640) {
    debug("in runDetectionsOnImageONNX");
    Net net;
    Mat frame;
    
    int retCode;
    retCode = setupDNN(modelWeights, net);
    if ( retCode != 0 ) {
        return retCode;
    }
    
    retCode = _runDetectionsOnImage(net, imagePath, cocoClassesFilePath, ds,
                                    confidenceThresholdMin=confidenceThresholdMin,
                                    confidenceThresholdMax=confidenceThresholdMax,
                                    inputSize=inputSize);
      
    debug(format("done with runDetectionsOnImageONNX, retCode: %d", retCode));
    return retCode;
}

int runDetectionsOnImageONNX(string imagePath, string modelWeights,
                             string cocoClassesFilePath,
                             PositionalFrameObjectDetectionDescriptor& pds,
                             double confidenceThresholdMin=0.1,
                             double confidenceThresholdMax=1.0,
                             int inputSize=640) {
    debug("in runDetectionsOnImageONNX");
    Net net;
    Mat frame;
    vector<ObjectDetectionDescriptor> ds;
    
    int retCode = runDetectionsOnImageONNX(imagePath, modelWeights, cocoClassesFilePath, ds,
                                           confidenceThresholdMin=confidenceThresholdMin,
                                           confidenceThresholdMax=confidenceThresholdMax,
                                           inputSize=inputSize);
    
    ObjectDetectionDescriptor arr[ds.size()];
    copy(ds.begin(), ds.end(), arr);

    pds = {};
    pds.size = ds.size();
    pds.position = 0;
    pds.detections = arr;
    
    debug(format("done with runDetectionsOnImageONNX, retCode: %d", retCode));
    return retCode;
}

int runDetectionsOnImage(string imagePath, string modelPath, string modelWeights,
                         string cocoClassesFilePath,
                         PositionalFrameObjectDetectionDescriptor& pds,
                         double confidenceThresholdMin=0.1,
                         double confidenceThresholdMax=1.0,
                         int inputSize=320) {
    debug("in runDetectionsOnImage");
    Net net;
    Mat frame;
    vector<ObjectDetectionDescriptor> ds;

    int retCode = 0;
    
    retCode = setupDNN(modelPath, modelWeights, net);
    if ( retCode != 0 ) {
        return retCode;
    }
    
    retCode = _runDetectionsOnImage(net, imagePath, cocoClassesFilePath, ds,
                                    confidenceThresholdMin=confidenceThresholdMin,
                                    confidenceThresholdMax=confidenceThresholdMax,
                                    inputSize=inputSize);

    pds = {};
    pds.size = ds.size();
    pds.position = 0;
    pds.detections = ds.data();

    debug(format("done with runDetectionsOnImage, retCode: %d", retCode));
    return retCode;
}

int _runDetectionsOnVideo(Net& net, string videoFilePath,
                          string cocoClassesFilePath,
                          FrameDetections& fd,
                         double confidenceThresholdMin=1.0,
                         double confidenceThresholdMax=0.1) {
    vector<Mat> frames;
    vector<string> cocoClasses;
    vector<PositionalFrameObjectDetectionDescriptor> detectionsPerFrame;
    int retCode;
    
    retCode = readAllFrames(videoFilePath, frames);
    if ( retCode != 0 ) {
        return retCode;
    }
    
    retCode = readFileToVectorOf(cocoClassesFilePath, cocoClasses);
    if ( retCode != 0 ) {
        return retCode;
    }
    
    for (long i = 0; i < frames.size(); i++ ) {
        vector<ObjectDetectionDescriptor> ds;
        runObjectDetectionsOn(frames[i], net, ds, cocoClasses,
                               confidenceThresholdMin=confidenceThresholdMin,
                               confidenceThresholdMax=confidenceThresholdMax);
        ObjectDetectionDescriptor arr[ds.size()];
        copy(ds.begin(), ds.end(), arr);
        detectionsPerFrame.push_back((PositionalFrameObjectDetectionDescriptor) {
            .position = static_cast<int>(i),
            .size = ds.size(),
            .detections = arr
        });
    }
    
    fd = (FrameDetections) {
        .size = detectionsPerFrame.size(),
        .frameDetections = detectionsPerFrame.data(),
    };
    
    debug(format("done with _runDetectionsOnVideo, retCode: %d", retCode));
    return retCode;
}

int runDetectionsOnVideoONNX(string videoFilePath, string modelWeights,
                         string cocoClassesFilePath,
                         FrameDetections& fd,
                         double confidenceThresholdMin=0.1,
                         double confidenceThresholdMax=1.0) {
    debug("in runDetectionsOnVideo");
    Net net;
    vector<Mat> frames;
    vector<PositionalFrameObjectDetectionDescriptor> detectionsPerFrame;
    vector<string> cocoClasses;
    int retCode = 0;
    
    retCode = setupDNN(modelWeights, net);
    if ( retCode != 0 ) {
        return retCode;
    }

    retCode = _runDetectionsOnVideo(net, videoFilePath, cocoClassesFilePath, fd,
                                    confidenceThresholdMin=confidenceThresholdMin,
                                    confidenceThresholdMax=confidenceThresholdMax);
    debug(format("done with runDetectionsOnVideo, retCode: %d", retCode));
    return retCode;
}

int runDetectionsOnVideo(string videoFilePath, string modelPath,
                         string modelWeights, string cocoClassesFilePath,
                         FrameDetections& fd,
                         double confidenceThresholdMin=0.1,
                         double confidenceThresholdMax=1.0) {
    debug("in runDetectionsOnVideo");
    Net net;
    vector<Mat> frames;
    vector<PositionalFrameObjectDetectionDescriptor> detectionsPerFrame;
    vector<string> cocoClasses;

    int retCode = 0;
    retCode = setupDNN(modelPath, modelWeights, net);
    if ( retCode != 0 ) {
        return retCode;
    }

    retCode = _runDetectionsOnVideo(net, videoFilePath, cocoClassesFilePath, fd,
                                    confidenceThresholdMin=confidenceThresholdMin,
                                    confidenceThresholdMax=confidenceThresholdMax);
    
    debug(format("done with runDetectionsOnVideo, retCode: %d", retCode));
    return retCode;
}
