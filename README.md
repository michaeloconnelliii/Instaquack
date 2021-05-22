<h1>InstaQuack!</h1>
<h2>Overview</h2>
<p>InstaQuack! is a mock social media server that provides users a way to communicate what they are doing using real time photos with short captions. The project uses the publish/subscribe model (see below) to allow data (photos) sharing between content creators (publishers) and subscribers. Publishers send their data to a broker which stores it under a given topic (specified by publisher). Subscribers can then receive the data from the broker for the topics they are subscribed to.</p>

![ScreenShot](/fig1.png)

<p>The Quacker Topic Store i​s where recently published photos are stored. The Quacker server serves as an intermediary for publishers and subscribers to store/receive photos. The server creates threads to perform work for publishers and subscribers. The interface facilitating client-server interaction is the series of command files for a given publisher or subscriber (client). The server gives these files to its appropriate threads (publisher or subscriber) and they interpret the files and perform the commands. Before this can happen, the server also has to be initialized using a main command file (which is interpreted and the initialization commands are performed). Finally, once the server has been initialized and the publisher/subscriber threads have been created, subscriber threads use the gathered data to create the ​InstaQuack Topic Web Pages (dynamic html pages). This allows a client to open each of the files and see what each subscriber was able to get from the topics.</p>

<p>For more implementation details, please look at my report collection (Report_Collection_Project3.pdf) and the original project specification (Project 3 - Description.pdf).</p>

<h2>Directory</h2>
<ul>
	<li>quacker.c<br>Project code.</li>
	<li>string_parser.c and string_parser.h<br>Contains the str_filler() method used throughout quacker.c which parses publisher, subscriber, and program commands. The other methods defined support str_filler().</li>
	<li>input (directory)<br>Publisher and subscriber commands</li>
	<li>input.txt<br>Program input. File used to create topics (queues), publishers, and subscribers.</li>
	<li>output (directory)<br>Sample dynamic html outputs (Topic Web Pages listed above)</li>
	<li>tests (directory)<br>Publisher and Subscriber tests (see section below) and screenshots verifying their results</li>
	<li>screenshots (directory)<br>Screenshots of successful compilation and successful valgrind report</li>
	<li>Project 3 - Description.pdf<br>Original specifications outlined for the project.</li>
	<li>Report_Collection_Project3.pdf<br>Summary/analysis of the project. Required in the project submission.</li>
	<li>Makefile<br>Simple Makefile recipe. Note that the pthread library is used.</li>
	<li>valgrind and program log.txt<br>Successful output from Valgrind while program was running. No memory leaks were found.</li>
</ul>

<h2>System Information</h2>
<p>InstaQuack was run, compiled, and tested on a virtual machine (via VirtualBox Version 6.0.24) running Linux Mint 19.3 MATE</p>

<h2>How to use</h2>
<ul> 
	<li>Makefile produces an executable, server.</li>
  	<li>To run (using the command line)
	  <ol>
	  	<li>Create executable (e.g. make will produce "server") and copy to "input" directory</li>
		<li>Navigate to the "input" directory</li>
		<li>./server input.txt</li>
		</ol>
	</li>
	<p>The output is a dynamically-created HTML page for each subscriber thread (of the form subscriber_<thread id>) listing photo captions they recieved during runtime. Running the program multiple times will result in slightly different HTML pages for each thread. This is intentional and evidence of the program being multithreaded.</p>
</ul>

<h2>Publisher and Subscriber Tests</h2>
<ul>
	<li>input_full_publisher_test.txt<br>successfully pushes all topic entries without dequeuing.</li>
	<li>pub_dont_give_up_.txt<br>Successfully continues to push all topic entries before dequeing after 30 seconds (delta = 30)</li>
	<li>sub_empty_test.txt<br>Unsuccessfully attempts to get topic entries from an empty queue. Program exits normally.</li>
	<li>normal_test (directory)<br>Screenshots from given project input.</li>
</ul>

<h2>Aknowledgements</h2>
<p>This project was for CIS 415, Operating Systems at University of Oregon, Fall 2020.</p> 
<p>Professor Allen Malony created the specifications for the project (Project 3 - Description.pdf) and Grayson Guan (the GTF for the class) created string_parser.c and string_parser.h.<p>
