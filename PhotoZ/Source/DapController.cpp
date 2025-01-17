//=============================================================================
// DapController.cpp
//=============================================================================
#include <iostream>
#include <stdlib.h>		// _gcvt()
#include <fstream>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <exception>
#include <stdint.h>
#include <FL/fl_ask.H>
#include <omp.h>
#include <unordered_map>

#include "NIDAQmx.h"
#include "DapController.h"
#include "UserInterface.h"
#include "DapChannel.h"
#include "Camera.h"
#include "DataArray.h"
#include "Definitions.h"

// #pragma comment(lib,".\\lib\\NIDAQmx.lib")  Chun suggested it but turns out not to make any difference
using namespace std;

/* hacky way of synchronizing things, but it seems to work and nothing better
 * was found
 */
#define CAM_INPUT_OFFSET 10
#define NUM_BNC_CHANNELS 4
#define DAQmxErrChk(functionCall)  if( DAQmxFailed(error=(functionCall)) ) NiErrorDump(error); else

 //=============================================================================
DapController::DapController()
{
	error = 0;
	char errBuff[2048] = { '\0' };
	reset = new DapChannel(0, 100);
	shutter = new DapChannel(0, 1210);
	sti1 = new DapChannel(300, 1);
	sti2 = new DapChannel(300, 1);

	// Acquisition
	acquiOnset = float(50);

	// Number of points per trace
	program = 7;
	numPts = 2000;

	intPts = 1000.0 / Camera::FREQ[program];

	// Flags
	stopFlag = 0;
	scheduleFlag = 0;
	scheduleRliFlag = 0;

	// Ch1
	numPulses1 = 1;
	intPulses1 = 10;

	numBursts1 = 1;
	intBursts1 = 200;

	// Ch2
	numPulses2 = 1;
	intPulses2 = 10;

	numBursts2 = 1;
	intBursts2 = 200;

	// Set Duration
	setDuration();
	
	// Default RLI settings
	darkPts = 200;
	lightPts = 280;

	// Live Feed Frame
	liveFeedFrame = NULL;
	liveFeedCam = NULL;

}


//=============================================================================
DapController::~DapController()
{
	delete reset;
	delete shutter;
	delete sti1;
	delete sti2;
	stop();
}

void NiErrorDump(int32 error) {
	char    errBuff[2048] = { '\0' };
	if (DAQmxFailed(error))
		DAQmxGetExtendedErrorInfo(errBuff, 2048);
	cout << errBuff;
}


int32 CVICALLBACK DoneCallback(TaskHandle taskHandle, int32 status, void* callbackData)
{
	int32   error = 0;
	char    errBuff[2048] = { '\0' };

	// Check to see if an error stopped the task.
	//DAQmxErrChk(status);

Error:
	if (DAQmxFailed(error)) {
		DAQmxGetExtendedErrorInfo(errBuff, 2048);
		DAQmxClearTask(taskHandle);
		fprintf(stdout, "DAQmx Error: %s\n", errBuff);
	}
	return 0;
}

//=============================================================================
// Number of Points per Trace
//=============================================================================
void DapController::setNumPts(int p)
{
	numPts = p;
}

//=============================================================================
int DapController::getNumPts()
{
	return numPts;
}

//=============================================================================
// Acquisition Onset
//=============================================================================
void DapController::setAcquiOnset(float p)
{
	acquiOnset = p;
}

//=============================================================================
float DapController::getAcquiOnset()
{
	return acquiOnset;
}

//=============================================================================
// Acquisition Duration
//=============================================================================
float DapController::getAcquiDuration()
{
	return (float)(numPts*intPts);
}

//=============================================================================
void DapController::setCameraProgram(int p)
{
	program = p;
	intPts = 1000.0 / (float)Camera::FREQ[program];
	//	int Frequency = Camera::FREQ[program];
}

//=============================================================================
int DapController::getCameraProgram()
{
	return program;
}

//=============================================================================
// Interval between Samples
//=============================================================================
void DapController::setIntPts(double p)
{
	intPts = p;
}

//=============================================================================
double DapController::getIntPts()
{
	return intPts;
}

//=============================================================================
// Acquisition
//=============================================================================
int DapController::acqui(unsigned short *memory, int16* fp_memory)
{
	Camera cam;
	cam.setCamProgram(getCameraProgram());
	cam.init_cam();

	//-------------------------------------------
	// Initialize NI tasks
	//int16* tmp_fp_memory = new(std::nothrow) int16[numPts * NUM_BNC_CHANNELS];
	//memset(tmp_fp_memory, 0, numPts * NUM_BNC_CHANNELS * sizeof(int16));
	float64 samplingRate = 1000.0 / getIntPts();
	NI_fillOutputs();

	//-------------------------------------------
	// Initialize variables for camera data management
	int width = cam.width();
	int height = cam.height();
	int quadrantSize = width * height;

	int superframe_factor = cam.get_superframe_factor();

	int32 defaultSuccess = -1;
	int32* successfulSamples = &defaultSuccess;
	int32 defaultReadSuccess = -1;
	int32* successfulSamplesIn = &defaultReadSuccess;

	//-------------------------------------------
	// Configure NI outputs and trigger
				// config clock channel M series don't have internal clock for output.
	// clk frequency calculation: see SM's BNC_ratio, BNC_R_list, output_rate, and frame_interval
	//			output_rate = BNC_ratio*1000.0 / frame_interval;
	if (!taskHandle_clk) {
		DAQmxErrChk(DAQmxCreateTask("Clock", &taskHandle_clk));
		DAQmxErrChk(DAQmxCreateCOPulseChanTime(taskHandle_clk, "Dev1/ctr0", "",
			DAQmx_Val_Seconds, DAQmx_Val_Low, 0.00, 0.50 / getIntPts(), 0.50 / getIntPts()));
		DAQmxErrChk(DAQmxCfgImplicitTiming(taskHandle_clk, DAQmx_Val_ContSamps, get_digital_output_size()));
	}
	// Stimulator outputs (line2) and Clock for synchronizing tasks w camera (line0)
	if (!taskHandle_out) {
		DAQmxErrChk(DAQmxCreateTask("Stimulators", &taskHandle_out));

		// To write a clock to trigger camera, open line0 channel also: "Dev1/port0/line0,Dev1/port0/line2" (and see NI_fillOutputs)
		DAQmxErrChk(DAQmxCreateDOChan(taskHandle_out, "Dev1/port0/line1:2", "", DAQmx_Val_ChanForAllLines));

		// Change this to "/Dev1/PFI12" for external trigger. But for now, trigger DO tasks from camera clock (PFI0)
		DAQmxErrChk(DAQmxCfgSampClkTiming(taskHandle_out, "/Dev1/PFI0", getIntPts(),
			DAQmx_Val_Rising, DAQmx_Val_FiniteSamps, get_digital_output_size()));
	}

	// External trigger:
	//DAQmxErrChk(DAQmxCfgDigEdgeStartTrig(taskHandle_clk, "/Dev1/PFI1", DAQmx_Val_Rising));

	//-------------------------------------------
	// Configure NI inputs and trigger
	if (!taskHandle_in) {
		DAQmxErrChk(DAQmxCreateTask("FP Input", &taskHandle_in));
		DAQmxErrChk(DAQmxCreateAIVoltageChan(taskHandle_in, "Dev1/ai0:3", "", DAQmx_Val_RSE, -10.0, 10.0, DAQmx_Val_Volts, NULL));
		DAQmxErrChk(DAQmxCfgSampClkTiming(taskHandle_in, "/Dev1/PFI0", float64(1005.0) / getIntPts(), // sync (cam clock) to trigger input
			DAQmx_Val_Rising, DAQmx_Val_FiniteSamps, 2 * (float64)get_digital_output_size() - (float64)getAcquiOnset()));
		//DAQmxErrChk(DAQmxCfgDigEdgeStartTrig(taskHandle_in, "/Dev1/PFI2", DAQmx_Val_Rising));
		DAQmxErrChk(DAQmxRegisterDoneEvent(taskHandle_clk, 0, DoneCallback, NULL));
	}
	//-------------------------------------------
	// Start NI tasks
	long total_written = 0, total_read = 0;

	DAQmxErrChk(DAQmxWriteDigitalU32(taskHandle_out, get_digital_output_size(), false, 0,
		DAQmx_Val_GroupByChannel, outputs, &total_written, NULL));

	DAQmxErrChk(DAQmxStartTask(taskHandle_in));
	DAQmxErrChk(DAQmxStartTask(taskHandle_out));
	DAQmxErrChk(DAQmxStartTask(taskHandle_clk));
	cout << "Total written per channel: " << total_written << "\n\t Size of output: " << get_digital_output_size() << "\n";

	/*
	DAQmxErrChk(DAQmxReadBinaryI16(taskHandle_in, numPts, 10, // timeout: 10 seconds to wait for samples?
		DAQmx_Val_GroupByScanNumber, tmp_fp_memory, 4 * numPts, successfulSamplesIn, NULL));
	*/


	//-------------------------------------------
	// Camera Acquisition loops
	//NI_openShutter(1);
	Sleep(100);
	int16* NI_ptr = fp_memory;
	omp_set_num_threads(NUM_PDV_CHANNELS);
#pragma omp parallel for	
	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {

		unsigned char* image;
		int loops = getNumPts() / superframe_factor; // superframing 

		// Start all images
		cam.start_images(ipdv, loops);

		unsigned short* privateMem = memory + (ipdv * quadrantSize * getNumPts()); // pointer to this thread's section of MEMORY	
		for (int i = 0; i < loops; i++)
		{
			// acquire data for this image from the IPDVth channel	
			image = cam.wait_image(ipdv);

			// Save the image(s) to process later	
			memcpy(privateMem, image, quadrantSize * sizeof(short) * superframe_factor);
			privateMem += quadrantSize * superframe_factor; // stride to the next destination for this channel's memory	

			if (ipdv == 0) {
				long read;
				int samplesSoFar = superframe_factor * (i + 1);
				if (i == loops - 1) {
					DAQmxErrChk(DAQmxReadBinaryI16(taskHandle_in, superframe_factor * (i + 1) - total_read, 5.0,
						DAQmx_Val_GroupByScanNumber, NI_ptr, (samplesSoFar - total_read + 1) * NUM_BNC_CHANNELS, &read, NULL));
				}
				else {
					DAQmxErrChk(DAQmxReadBinaryI16(taskHandle_in, superframe_factor * (i + 1) - total_read, 0.01,
						DAQmx_Val_GroupByScanNumber, NI_ptr, (samplesSoFar - total_read) * NUM_BNC_CHANNELS, &read, NULL));
				}
				NI_ptr += read * NUM_BNC_CHANNELS;

				total_read += read;
			}
		}
		cam.end_images(ipdv);
	}
	//NI_openShutter(0);
	cout << "Total read: " << total_read << "\n";

	//=============================================================================	
	// NI clean up
	delete[] outputs;

	NI_stopTasks();
	NI_clearTasks();

	//=============================================================================	
	// Image reassembly	
	cam.reassembleImages(memory, numPts);

	//=============================================================================	
	// FP reassembly
	/*
	int16* dst_fp = fp_memory;
	for (int i_bnc = 0; i_bnc < NUM_BNC_CHANNELS; i_bnc++) {
		int16* src_fp = tmp_fp_memory + i_bnc;
		for (int m = 0; m < total_read; m++) {
			*dst_fp++ = *src_fp;
			src_fp += NUM_BNC_CHANNELS;
		}
		dst_fp += numPts - total_read; // skip to next FP trace start (nonzero iff missing read)
	}
	*/

	//delete[] tmp_fp_memory;


	return 0;
}

//=============================================================================
int DapController::stop()
{
	stopFlag = 1;
	NI_stopTasks();
	NI_clearTasks();
	return  0;
}

//=============================================================================
void DapController::setDuration()
{
	float time;

	time = acquiOnset + int(getAcquiDuration()) + 1;
	duration = time;

	time = reset->getOnset() + reset->getDuration();
	if (time > duration)
		duration = time;

	time = shutter->getOnset() + shutter->getDuration();
	if (time > duration)
		duration = time;

	time = sti1->getOnset() + sti1->getDuration() + (numPulses1 - 1)*intPulses1 + (numBursts1 - 1)*intBursts1;
	if (time > duration)
		duration = time;

	time = sti2->getOnset() + sti2->getDuration() + (numPulses2 - 1)*intPulses2 + (numBursts2 - 1)*intBursts2;
	if (time > duration)
		duration = time;

	duration++;

	if (duration > 60000)
	{
		fl_alert("DC setDuration The total duration of the acquisition can not exceed 1 min! Please adjust DAP settings.");
		return;
	}
}

float DapController::getDuration() {
	return duration;
}


//=============================================================================
void DapController::setStopFlag(char p)
{
	stopFlag = p;
}

//=============================================================================
char DapController::getStopFlag()
{
	return stopFlag;
}

//=============================================================================
int DapController::takeRli(unsigned short* memory) {

	Camera cam;

	cam.setCamProgram(getCameraProgram());
	cam.init_cam();

	int rliPts = darkPts + lightPts;

	int width = cam.width();
	int height = cam.height();
	int quadrantSize = width * height;

	int superframe_factor = cam.get_superframe_factor();

	omp_set_num_threads(NUM_PDV_CHANNELS);
	// acquire dark frames with LED off	
#pragma omp parallel for	
	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
		unsigned char* image;
		int loops = darkPts / superframe_factor; // superframing 

		// Start all images
		cam.start_images(ipdv, loops);

		unsigned short* privateMem = memory + (ipdv * quadrantSize * rliPts); // pointer to this thread's section of MEMORY	
		for (int i = 0; i < loops; i++)
		{
			// acquire data for this image from the IPDVth channel	
			image = cam.wait_image(ipdv);

			// Save the image(s) to process later	
			memcpy(privateMem, image, quadrantSize * sizeof(short) * superframe_factor);
			privateMem += quadrantSize * superframe_factor; // stride to the next destination for this channel's memory	
		}
	}
	// parallel section closes momentarily, threads sync and close, then split up again.
	NI_openShutter(1);
	Sleep(100);
	omp_set_num_threads(NUM_PDV_CHANNELS);
	// parallel acquisition resumes now that light is on	
#pragma omp parallel for	
	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
		unsigned char* image;
		int loops = lightPts / superframe_factor; // superframing 

		cam.start_images(ipdv, loops);

		unsigned short* privateMem = memory + (ipdv * quadrantSize * rliPts) // pointer to this thread's section of MEMORY	
			+ (quadrantSize * darkPts); // offset of where we left off	

		for (int i = 0; i < loops; i++) 		// acquire rest of frames with LED on	
		{
			// acquire data for this image from the IPDVth channel	
			image = cam.wait_image(ipdv);

			// Save the image(s) to process later	
			memcpy(privateMem, image, quadrantSize * sizeof(short) * superframe_factor);
			privateMem += quadrantSize * superframe_factor; // stride to the next destination for this channel's memory	

		}
		cam.end_images(ipdv);
	}
	NI_openShutter(0); // light off	
	NI_stopTasks();
	NI_clearTasks();

	//=============================================================================	
	// Image reassembly	
	cam.reassembleImages(memory, rliPts); // deinterleaves, CDS subtracts, and arranges data

	// Debug: print reassembled images out
	/*
	unsigned short* img = (unsigned short*)(memory);
	img += 355 * quadrantSize * NUM_PDV_CHANNELS / 2; // stride to the full image (now 1/2 size due to CDS subtract)


	std::string filename = "full-out355.txt";
	cam.printFinishedImage(img, filename.c_str(), true);
	cout << "\t This full image was located in MEMORY at offset " <<
		(img - (unsigned short*)memory) / quadrantSize << " quadrant-sizes\n";
	*/

	cam.set_freerun_mode();

	return 0;
}


//=============================================================================
void DapController::NI_fillOutputs()
{

	float start, end;
	size_t do_size = get_digital_output_size();
	int num_DO_channels = 1; // number of DO channels in the DO task 
	outputs = new uInt32[do_size * num_DO_channels];

	//--------------------------------------------------------------
	// Reset the array
	memset(outputs, 0, sizeof(uInt32) * do_size * num_DO_channels);
	//--------------------------------------------------------------
	// Shutter (can instead be handled as a simple separate task, since exact sync not needed)
	uInt32 shutter_mask = 1 << 1; //line1
	uInt32 stim1_mask = 1 << 2; //line2

	start = getShutterOnset() / intPts;


	for (int i = (int)start; i < do_size - 1; i++) {
		outputs[i] |= shutter_mask;
	}
	//--------------------------------------------------------------

	// If we want a clock to trigger camera, write this to line0
	/*
	uInt8 resting_voltage = 1;
	uInt8 trigger_voltage = 0;
	// If BNC_ratio > 1, the resting/triggering voltages are switched

	// Assuming BNC ratio == 1:
	for (int i = 0; i < do_size; i++) {
		outputs[i] = trigger_voltage;
	}*/

	// Stimulator #1
	cout << "\n\tNum bursts 1: " << numBursts1 << "\n\tNum Pulses 1: " << numPulses1 << "\n";
	cout << "\n\tInt bursts 1: " << intBursts1 << "\n\tInt Pulses 1: " << intPulses1 << "\n";
	cout << "\n\tOnset 1: " << sti1->getOnset() << "\n";
	cout << "\n\Duration 1: " << sti1->getDuration() << "\n";
	for (int k = 0; k < numBursts1; k++)
	{
		for (int j = 0; j < numPulses1; j++)
		{
			start = sti1->getOnset() / intPts + j * intPulses1 + k * intBursts1;
			end = (start + sti1->getDuration() / intPts);
			for (int i = (int)start; i < end; i++) {
				cout << "stim 1 is high at " << i << "\n";
				outputs[i] |= stim1_mask;
			}
		}
	}
	//--------------------------------------------------------------
	// Stimulator #2
	/*
	cout << "\n\tNum bursts 2: " << numBursts2 << "\n\tNum Pulses 2: " << numPulses2 << "\n";
	cout << "\n\tInt bursts 2: " << intBursts2 << "\n\tInt Pulses 2: " << intPulses2 << "\n";
	cout << "\n\tOnset 2: " << sti2->getOnset() << "\n";
	cout << "\n\Duration 2: " << sti2->getDuration() << "\n";
	for (int k = 0; k < numBursts2; k++)
	{
		for (int j = 0; j < numPulses2; j++)
		{
			start = sti2->getOnset() / intPts + j * intPulses2 + k * intBursts2;
			end = (start + sti2->getDuration() / intPts);
			for (int i = (int)start; i < end; i++) {
				cout << "stim 2 is high at " << i << "\n";
				outputs[i + 2 * do_size] = 1; // |= stim2_mask;
			}
		}
	}*/

	// Add new stimulators or stimulation features and patterns here

}

//=============================================================================
//============================  Live Feed  ====================================
void DapController::startLiveFeed(unsigned short* frame, bool* flags) {
	liveFeedFrame = frame;
	liveFeedFlags = flags;
}

// This is launched by Python application as a separate thread (sep from GUI and plotter daemons)
void DapController::continueLiveFeed() {
	// populate liveFeedFrame with the next image.
	if (!liveFeedCam) return;
	int width = liveFeedCam->width();
	int height = liveFeedCam->height();
	int quadrantSize = width * height;

	// Temporary memory for storing last image
	unsigned short* tmp = new unsigned short[(size_t)quadrantSize * NUM_PDV_CHANNELS];

	NI_openShutter(1);

	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
		liveFeedCam->start_images(ipdv, 0); // start free run
	}
	unsigned char* images[NUM_PDV_CHANNELS];
	// Gather first image just for reset rows.
	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
		images[ipdv] = liveFeedCam->wait_image(ipdv);
		memcpy(tmp + quadrantSize * ipdv, images[ipdv], quadrantSize * sizeof(short));
	}

	while (!liveFeedFlags[1]) {

		// Move current frame into first position to make use of CDS subtract rows only
		for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
			memcpy(liveFeedFrame + (quadrantSize * ipdv * 2), tmp + quadrantSize * ipdv, quadrantSize * sizeof(short));
		}

		// Gather second image to display. Keep in channel-major order
		for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
			images[ipdv] = liveFeedCam->wait_image(ipdv);
			memcpy(liveFeedFrame + (quadrantSize * (ipdv * 2 + 1)), images[ipdv], quadrantSize * sizeof(short));
			memcpy(tmp + quadrantSize * ipdv, images[ipdv], quadrantSize * sizeof(short)); // and for next image also
		}

		liveFeedCam->reassembleImages(liveFeedFrame, 2);

		//memcpy(liveFeedFrame, liveFeedFrame + quadrantSize * NUM_PDV_CHANNELS / 2, quadrantSize * NUM_PDV_CHANNELS * sizeof(short) / 2);
		liveFeedFlags[0] = true;

		// Debug -- output to file
		//std::string filename = "full-out1.txt";
		//liveFeedCam->printFinishedImage(liveFeedFrame, filename.c_str(), true);

		while (liveFeedFlags[0] and !liveFeedFlags[1]) { // wait for plotter to be ready for next image
			Sleep(10);
		}

	}
	cout << "Acqui daemon has read stop-loop flag, stopping.\n";

	for (int ipdv = 0; ipdv < NUM_PDV_CHANNELS; ipdv++) {
		liveFeedCam->start_images(ipdv, 1); // end free run
		liveFeedCam->end_images(ipdv);
	}

	stopLiveFeed(); // prepare for later hardware use
}

void DapController::stopLiveFeed() {
	NI_openShutter(0);
	NI_stopTasks();
	NI_clearTasks();
	if (liveFeedCam) {
		delete liveFeedCam;
	}
	liveFeedCam = NULL;
	liveFeedFrame = NULL; 

	liveFeedFlags[1] = false;
}
//=============================================================================

void DapController::resetCamera()
{
	Camera cam;
	for (int ipdv = 0; ipdv < 4; ipdv++) {
		cam.end_images(ipdv);
	}
	cam.close_channels();

	cam.init_cam();
}

//=============================================================================
void DapController::setNumPulses(int ch, int p) {
	if (ch == 1) numPulses1 = p;
	else numPulses2 = p;
}

//=============================================================================
void DapController::setNumBursts(int ch, int num) {
	if (ch == 1) numBursts1 = num;
	else numBursts2 = num;
}

//=============================================================================
int DapController::getNumBursts(int ch) {
	if (ch == 1) return numBursts1;
	return numBursts2;
}

//=============================================================================
int DapController::getNumPulses(int ch) {
	if (ch == 1) return numPulses1;
	return numPulses2;
}

//=============================================================================
void DapController::setIntBursts(int ch, int p) {
	if (ch == 1) intBursts1 = p;
	else intBursts2 = p;
}

//=============================================================================
void DapController::setIntPulses(int ch, int p) {
	if (ch == 1) intPulses1 = p;
	else intPulses2 = p;
}

//=============================================================================
int DapController::getIntBursts(int ch) {
	if (ch == 1) return intBursts1;
	return intBursts2;
}

//=============================================================================
int DapController::getIntPulses(int ch) {
	if (ch == 1) return intPulses1;
	return intPulses2;
}

//=============================================================================
void DapController::setScheduleFlag(char p) {
	scheduleFlag = p;
}

//=============================================================================
void DapController::setScheduleRliFlag(char p) {
	scheduleRliFlag = p;
}

//=============================================================================
char DapController::getScheduleFlag() {
	return scheduleFlag;
}

//=============================================================================
char DapController::getScheduleRliFlag() {
	return scheduleRliFlag;
}

void DapController::setShutterOnset(float v) {
	shutter->setOnset(v);
}

float DapController::getShutterOnset() {
	return shutter->getOnset();
}

//=============================================================================
int DapController::NI_openShutter(uInt8 on)
{
	int32       error = 0;
	TaskHandle  taskHandle = 0;
	uInt8       data[4] = { 0,on,0,0 };
	char        errBuff[2048] = { '\0' };

	DAQmxErrChk(DAQmxCreateTask("", &taskHandle));
	DAQmxErrChk(DAQmxCreateDOChan(taskHandle, "Dev1/port0/line0:1", "", DAQmx_Val_ChanForAllLines));
	DAQmxErrChk(DAQmxStartTask(taskHandle));
	DAQmxErrChk(DAQmxWriteDigitalLines(taskHandle, 1, 1, 10.0, DAQmx_Val_GroupByChannel, data, NULL, NULL));

Error:
	if (DAQmxFailed(error))
		DAQmxGetExtendedErrorInfo(errBuff, 2048);
	if (taskHandle != 0) {
		DAQmxStopTask(taskHandle);
		DAQmxClearTask(taskHandle);
	}
	if (DAQmxFailed(error))
		printf("DAQmx Error: %s\n", errBuff);
	return 0;
}

//=============================================================================
void DapController::NI_stopTasks()
{
	if (taskHandle_in) DAQmxStopTask(taskHandle_in);
	if (taskHandle_out) DAQmxStopTask(taskHandle_out);
	if (taskHandle_clk) DAQmxStopTask(taskHandle_clk);
	if (taskHandle_led) DAQmxStopTask(taskHandle_led);
}

//=============================================================================
void DapController::NI_clearTasks()
{
	if (taskHandle_in) DAQmxClearTask(taskHandle_in);
	if (taskHandle_out) DAQmxClearTask(taskHandle_out);
	if (taskHandle_clk) DAQmxClearTask(taskHandle_clk);
	if (taskHandle_led) DAQmxClearTask(taskHandle_led);

	taskHandle_led = NULL;
	taskHandle_clk = NULL;
	taskHandle_in = NULL;
	taskHandle_out = NULL;
}

size_t DapController::get_digital_output_size() {
	return numPts;
	//return (size_t)duration + 10;
}

//=============================================================================

//Concerns:
//Defining functions in files (like .dap files) which can send the signals to NI
//Dap820Put is used to send system commands. Figure out port equivalent to SYSin
//(or check if it's even needed as tasks can define and what needs to be done and
//  when executed will automatically send signals for niboards ports to the LED and STIMULATOR)
//Understand the code in .dap files.
//Burst mode usage

//Done (probably):
//Equivalent of dap handle to comm pipe is channel affiliated with a task