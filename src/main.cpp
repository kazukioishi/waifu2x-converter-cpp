/*
 * main.cpp
 *   (ここにファイルの簡易説明を記入)
 *
 *  Created on: 2015/05/24
 *      Author: wlamigo
 * 
 *   (ここにファイルの説明を記入)
 */

#include <opencv2/opencv.hpp>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <cmath>
#include <sys/time.h>
#include "picojson.h"
#include "tclap/CmdLine.h"

#include "modelHandler.hpp"
#include "main.hpp"
using namespace cv;

int main(int argc, char** argv) {

	// definition of command line arguments
	TCLAP::CmdLine cmd("waifu2x reimplementation using OpenCV", ' ', "1.0.0");

	TCLAP::ValueArg<std::string> cmdInputFile("i", "input_file",
			"path to input image file (you should input full path)", true, "",
			"string", cmd);

	TCLAP::ValueArg<std::string> cmdOutputFile("o", "output_file",
			"path to output image file (you should input full path)", false,
			"(auto)", "string", cmd);

	std::vector<std::string> cmdModeConstraintV;
	cmdModeConstraintV.push_back("noise");
	cmdModeConstraintV.push_back("scale");
	cmdModeConstraintV.push_back("noise_scale");
	TCLAP::ValuesConstraint<std::string> cmdModeConstraint(cmdModeConstraintV);
	TCLAP::ValueArg<std::string> cmdMode("m", "mode", "image processing mode",
			false, "noise_scale", &cmdModeConstraint, cmd);

	std::vector<int> cmdNRLConstraintV;
	cmdNRLConstraintV.push_back(1);
	cmdNRLConstraintV.push_back(2);
	TCLAP::ValuesConstraint<int> cmdNRLConstraint(cmdNRLConstraintV);
	TCLAP::ValueArg<int> cmdNRLevel("", "noise_level", "noise reduction level",
			false, 1, &cmdNRLConstraint, cmd);

	TCLAP::ValueArg<double> cmdScaleRatio("", "scale_ratio",
			"custom scale ratio", false, 2.0, "double", cmd);

	TCLAP::ValueArg<std::string> cmdModelPath("", "model_dir",
			"path to custom model directory (don't append last / )", false,
			"models", "string", cmd);

	TCLAP::ValueArg<int> cmdNumberOfJobs("j", "jobs",
			"number of threads launching at the same time", false, 4, "integer",
			cmd);

    TCLAP::SwitchArg cmdUseCL("", "useopencl", "Use the OpenCL acceleration.", cmd, false);

	// definition of command line argument : end

	// parse command line arguments
	try {
		cmd.parse(argc, argv);
	} catch (std::exception &e) {
		std::cerr << e.what() << std::endl;
		std::cerr << "Error : cmd.parse() threw exception" << std::endl;
		std::exit(-1);
	}

    //set useOpenCL
	if(cmdUseCL.getValue()){
		std::cout << "Using OpenCL...." << std::endl;
		UseOpenCL = true;
	}
	//timer
	struct timeval startTime, finishTime;
	gettimeofday(&startTime, NULL);

	// load image file
	cv::Mat image = cv::imread(cmdInputFile.getValue(), cv::IMREAD_COLOR);
	image.convertTo(image, CV_32F, 1.0 / 255.0);

	// ===== Noise Reduction Phase =====
	if (cmdMode.getValue() == "noise" || cmdMode.getValue() == "noise_scale") {
		std::string modelFileName(cmdModelPath.getValue());
		modelFileName = modelFileName + "/noise"
				+ std::to_string(cmdNRLevel.getValue()) + "_model.json";
		std::vector<std::unique_ptr<w2xc::Model> > models;

		if (!w2xc::modelUtility::generateModelFromJSON(modelFileName, models))
			std::exit(-1);

		// set njob
		for (auto&& model : models) {
			model->setNumberOfJobs(cmdNumberOfJobs.getValue());
		}

		cv::Mat imageYUV;
		cv::cvtColor(image, imageYUV, COLOR_RGB2YUV);
		std::vector<cv::Mat> imageSplit;
		cv::Mat imageY;
		cv::split(imageYUV, imageSplit);
		imageSplit[0].copyTo(imageY);

		std::unique_ptr<std::vector<cv::Mat> > inputPlanes = std::unique_ptr<
				std::vector<cv::Mat> >(new std::vector<cv::Mat>());
		std::unique_ptr<std::vector<cv::Mat> > outputPlanes = std::unique_ptr<
				std::vector<cv::Mat> >(new std::vector<cv::Mat>());

		inputPlanes->clear();
		inputPlanes->push_back(imageY);

		std::cout << "start noise reduction (level "
				<< std::to_string(cmdNRLevel.getValue()) << ")" << std::endl;

		for (int index = 0; index < models.size(); index++) {
			std::cout << "Iteration #" << (index + 1) << "..." << std::endl;
			if (!models[index]->filter(*inputPlanes, *outputPlanes)) {
				std::exit(-1);
			}
			if (index != models.size() - 1) {
				inputPlanes = std::move(outputPlanes);
				outputPlanes = std::unique_ptr<std::vector<cv::Mat> >(
						new std::vector<cv::Mat>());
			}
		}

		outputPlanes->at(0).copyTo(imageSplit[0]);
		cv::merge(imageSplit, imageYUV);
		cv::cvtColor(imageYUV, image, COLOR_YUV2RGB);

	} // noise reduction phase : end

	// ===== scaling phase =====

	if (cmdMode.getValue() == "scale" || cmdMode.getValue() == "noise_scale") {

		// calculate iteration times of 2x scaling and shrink ratio which will use at last
		int iterTimesTwiceScaling = static_cast<int>(std::ceil(
				std::log2(cmdScaleRatio.getValue())));
		double shrinkRatio = 0.0;
		if (static_cast<int>(cmdScaleRatio.getValue())
				!= std::pow(2, iterTimesTwiceScaling)) {
			shrinkRatio = cmdScaleRatio.getValue()
					/ std::pow(2.0, static_cast<double>(iterTimesTwiceScaling));
		}

		std::string modelFileName(cmdModelPath.getValue());
		modelFileName = modelFileName + "/scale2.0x_model.json";
		std::vector<std::unique_ptr<w2xc::Model> > models;

		if (!w2xc::modelUtility::generateModelFromJSON(modelFileName, models))
			std::exit(-1);

		// set njob
		for (auto&& model : models) {
			model->setNumberOfJobs(cmdNumberOfJobs.getValue());
		}

		std::cout << "start scaling" << std::endl;

		// 2x scaling
		for (int nIteration = 0; nIteration < iterTimesTwiceScaling;
				nIteration++) {

			std::cout << "#" << std::to_string(nIteration + 1)
					<< " 2x scaling..." << std::endl;

			cv::Mat imageYUV;
			cv::Size imageSize = image.size();
			imageSize.width *= 2;
			imageSize.height *= 2;
			cv::Mat image2xNearest;
			cv::resize(image, image2xNearest, imageSize, 0, 0, INTER_NEAREST);
			cv::cvtColor(image2xNearest, imageYUV, COLOR_RGB2YUV);
			std::vector<cv::Mat> imageSplit;
			cv::Mat imageY;
			cv::split(imageYUV, imageSplit);
			imageSplit[0].copyTo(imageY);

			// generate bicubic scaled image and
			// convert RGB -> YUV and split
			imageSplit.clear();
			cv::Mat image2xBicubic;
			cv::resize(image,image2xBicubic,imageSize,0,0,INTER_CUBIC);
			cv::cvtColor(image2xBicubic, imageYUV, COLOR_RGB2YUV);
			cv::split(imageYUV, imageSplit);

			std::unique_ptr<std::vector<cv::Mat> > inputPlanes =
					std::unique_ptr<std::vector<cv::Mat> >(
							new std::vector<cv::Mat>());
			std::unique_ptr<std::vector<cv::Mat> > outputPlanes =
					std::unique_ptr<std::vector<cv::Mat> >(
							new std::vector<cv::Mat>());

			inputPlanes->clear();
			inputPlanes->push_back(imageY);

			for (int index = 0; index < models.size(); index++) {
				std::cout << "Iteration #" << (index + 1) << "..." << std::endl;
				if (!models[index]->filter(*inputPlanes, *outputPlanes)) {
					std::exit(-1);
				}
				if (index != models.size() - 1) {
					inputPlanes = std::move(outputPlanes);
					outputPlanes = std::unique_ptr<std::vector<cv::Mat> >(
							new std::vector<cv::Mat>());
				}
			}

			outputPlanes->at(0).copyTo(imageSplit[0]);
			cv::merge(imageSplit, imageYUV);
			cv::cvtColor(imageYUV, image, COLOR_YUV2RGB);

		} // 2x scaling : end

		if (shrinkRatio != 0.0) {
			cv::Size lastImageSize = image.size();
			lastImageSize.width =
					static_cast<int>(static_cast<double>(lastImageSize.width
							* shrinkRatio));
			lastImageSize.height =
					static_cast<int>(static_cast<double>(lastImageSize.height
							* shrinkRatio));
			cv::resize(image, image, lastImageSize, 0, 0, INTER_LINEAR);
		}

	}

	image.convertTo(image, CV_8U, 255.0);
	std::string outputFileName = cmdOutputFile.getValue();
	if (outputFileName == "(auto)") {
		outputFileName = cmdInputFile.getValue();
		int tailDot = outputFileName.find_last_of('.');
		outputFileName.erase(tailDot, outputFileName.length());
		outputFileName = outputFileName + "(" + cmdMode.getValue() + ")";
		std::string &mode = cmdMode.getValue();
		if(mode.find("noise") != mode.npos){
			outputFileName = outputFileName + "(Level" + std::to_string(cmdNRLevel.getValue())
			+ ")";
		}
		if(mode.find("scale") != mode.npos){
			outputFileName = outputFileName + "(x" + std::to_string(cmdScaleRatio.getValue())
			+ ")";
		}
		outputFileName += ".png";
	}
	cv::imwrite(outputFileName, image);
	
	gettimeofday(&finishTime, NULL);
	std::cout << (finishTime.tv_sec - startTime.tv_sec) * 1000 + (finishTime.tv_usec - startTime.tv_usec) / 1000 << "msec." << std::endl;
	std::cout << "process successfully done!" << std::endl;
	
	return 0;
}

