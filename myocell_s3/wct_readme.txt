For EMG applications, the WCT (Wilson Central Terminal) pin on the ADS1298 is generally not used and should be left unconnected (floating) or connected to a test point. Instead, EMG requires the Right Leg Drive (RLD) circuit for common-mode rejection. Do not connect the WCT pin to ground, as this can cause a short circuit. 
Key Connections for EMG on ADS1298:
WCT Pin: Leave unconnected (do not ground).
Reference/Ground (RLD): Connect the RLD electrode to the RLD_OUT pin.
RLD Feedback: Short RLD_OUT and RLD_INV together externally.
Channel Inputs: Configure electrodes to differential inputs, typically mapping to INxP and INxN for each EMG channel. 
Configuration Details:
Use the RLD amplifier (not WCT) to generate the common-mode signal.
Ensure the RLD amplifier is configured in the registers to use the electrodes (RA/LA/LL or electrode inputs) to derive the common-mode voltage.
The ADS1298, unlike the ADS1299, does not require the SRB (Signal Reference Buffer) for common-mode referencing; use direct differential input routing for best results. 